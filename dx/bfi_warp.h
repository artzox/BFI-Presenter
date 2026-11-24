// ============================================================================
// BFI Presenter -- frame warp, Direct3D 12 side: motion between the last two frames, measured on the GPU
//
// Copyright (C) 2026 artzox -- https://github.com/artzox/BFI-Presenter
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the presenter needs to show a frame moved ahead along its own motion (see common/bfi_warpclock.h for when
// and how far):
//
//   1. Each new frame, once: its brightness at 1/4 size, and four more levels each half the size before (a pyramid).
//      One pass over the frame and four tiny ones. The pyramid of the frame shown before is kept.
//   2. Only when a frame is about to be shown moved: where each part of the newer frame was in the older one,
//      coarse to fine. The smallest level tries every offset within 3 pixels (192 pixels of the game frame at 4K);
//      each finer level starts from the level above (its own vector and its four neighbours', and no motion at all)
//      and refines by a pixel, then half a pixel (the last level down to an eighth). Where the last level finds the
//      picture unchanged from the frame before, the motion is none: a HUD stays put. Matching is by a 9-tap patch. The result is one vector per 8x8
//      (or 4x4) block of the game frame with a confidence: where nothing matched (uncovered background, a cut) the
//      picture is left where it is.
//      A last pass evens the vectors out among neighbours that agree (a block's match is only good to a pixel or two).
//   3. The per-refresh shader reads that texture (t3) and takes each pixel from further back along its motion --
//      except what is the same pixel for pixel in the frame shown before (t4): that stands still on the screen
//      (HUD, text, crosshair) and is shown as rendered.
//
// No game data is needed (no motion vectors, depth or camera): it works on the finished picture, HUD and ReShade
// effects included -- and has their limits: see docs/settings-guide.md, "Frame warp". Where the game hands its
// upscaler its motion vectors and depth (Streamline), PSWarpGame below makes the motion texture from those instead.
//
// Uses the presenter's root signature (b0 constants, t0-t4 one SRV each, s0 linear clamp) and its vertex shader.
// ============================================================================
#pragma once

static const char kWarpShader[] = R"HLSL(
cbuffer W : register(b0)
{
    float2 texel;    // 1 / size of the level being drawn
    float2 texel2;   // brightness pass: 1 / size of the game frame; motion pass: 1 / size of the coarser level;
                     // evening-out pass for whole-picture motion: 1 / size of the level the vectors come from
    uint   wmode;    // brightness pass: 1 = the frame is linear (scRGB); motion pass: 1 = start from the coarser level,
                     // +2 = the last level. Evening-out pass: 1 = whole-picture motion
};
Texture2D<float4> ta : register(t0);   // brightness: the game frame / the finer level. Motion: this level, newer frame
Texture2D<float4> tb : register(t1);   // motion: this level, older frame
Texture2D<float4> tc : register(t2);   // motion: the coarser level's vectors
SamplerState linClamp : register(s0);

float WLuma(float3 c)
{
    float y = dot(max(c, 0.0), float3(0.2126, 0.7152, 0.0722));
    return wmode == 1 ? sqrt(y / (y + 1.0)) : y;       // linear light: compressed, so dark and bright areas match alike
}

// The frame's brightness at a quarter of its size: 4 bilinear taps, each the mean of 2x2 pixels.
float4 PSWarpLuma(float4 pos : SV_Position) : SV_Target
{
    float2 uv = pos.xy * texel;
    float y = WLuma(ta.SampleLevel(linClamp, uv + texel2 * float2(-1.0, -1.0), 0).rgb)
            + WLuma(ta.SampleLevel(linClamp, uv + texel2 * float2( 1.0, -1.0), 0).rgb)
            + WLuma(ta.SampleLevel(linClamp, uv + texel2 * float2(-1.0,  1.0), 0).rgb)
            + WLuma(ta.SampleLevel(linClamp, uv + texel2 * float2( 1.0,  1.0), 0).rgb);
    return float4(0.25 * y, 0.0, 0.0, 1.0);
}

// The next level: half the size (one bilinear tap between four pixels).
float4 PSWarpDown(float4 pos : SV_Position) : SV_Target
{
    return float4(ta.SampleLevel(linClamp, pos.xy * texel, 0).r, 0.0, 0.0, 1.0);
}

#define PULL 0.05
static const float2 kTap[9] = {
    float2(-1.5, -1.5), float2(0.0, -1.5), float2(1.5, -1.5),
    float2(-1.5,  0.0), float2(0.0,  0.0), float2(1.5,  0.0),
    float2(-1.5,  1.5), float2(0.0,  1.5), float2(1.5,  1.5) };

// How badly the patch around uv in the newer frame matches the older frame f further back (f in uv).
float PatchCost(float2 uv, float2 f, float cur[9])
{
    float c = 0.0;
    for (int i = 0; i < 9; i++) c += abs(cur[i] - tb.SampleLevel(linClamp, uv + kTap[i] * texel - f, 0).r);
    return c;
}

// Motion: xy = how far the picture at this point moved from the older frame to the newer one (uv), z = confidence.
float4 PSWarpFlow(float4 pos : SV_Position) : SV_Target
{
    float2 uv = pos.xy * texel;
    float cur[9];
    for (int i = 0; i < 9; i++) cur[i] = ta.SampleLevel(linClamp, uv + kTap[i] * texel, 0).r;
    float2 best = float2(0.0, 0.0);
    float2 lead = float2(0.0, 0.0);                      // what the level above found here
    float bestC = PatchCost(uv, best, cur);              // standing still
    // The last level: where the picture is the same as in the frame before, nothing moved -- or nothing that can be
    // seen moving (a flat area). A HUD over a moving scene stays exactly where it is, fine text included, which the
    // levels above can't tell from its background.
    // (w = 0: no motion was measured here; whole-picture motion leaves these out of its average.)
    if ((wmode & 2) != 0 && bestC < 0.03) return float4(0.0, 0.0, 1.0, 0.0);
    if ((wmode & 1) == 0)
    {
        // the smallest level: standing still unless an offset is clearly better
        for (int y = -3; y <= 3; y++)
        {
            for (int x = -3; x <= 3; x++)
            {
                float2 o = float2(x, y);
                float c = PatchCost(uv, o * texel, cur) + 0.01 + 0.004 * length(o);
                if (c < bestC) { bestC = c; best = o * texel; }
            }
        }
    }
    else
    {
        static const float2 kN[5] = { float2(0.0, 0.0), float2(1.0, 0.0), float2(-1.0, 0.0), float2(0.0, 1.0), float2(0.0, -1.0) };
        // The level above already found this area's motion: it is kept unless standing still or a neighbour's
        // motion matches clearly better. (Preferring "still" here left low-contrast areas behind in a pan.)
        bestC += 0.01;
        for (int k = 0; k < 5; k++)
        {
            float2 f = tc.SampleLevel(linClamp, uv + kN[k] * texel2, 0).xy;
            if (k == 0) lead = f;
            float c = PatchCost(uv, f, cur) + (k == 0 ? 0.0 : 0.01);
            if (c < bestC) { bestC = c; best = f; }
        }
    }
    static const float2 kR[8] = { float2(-1.0, -1.0), float2(0.0, -1.0), float2(1.0, -1.0), float2(-1.0, 0.0),
                                  float2(1.0, 0.0), float2(-1.0, 1.0), float2(0.0, 1.0), float2(1.0, 1.0) };
    // a pixel, then half a pixel; the last level goes on to an eighth (one pixel of the game frame at 1/8 size)
    float stepPx = 1.0;
    int rounds = (wmode & 2) != 0 ? 4 : 2;
    for (int r = 0; r < rounds; r++)
    {
        float2 base = best;
        for (int k = 0; k < 8; k++)
        {
            float2 f = base + kR[k] * (stepPx * texel);
            // Going more than half a pixel from what the level above found costs a little per pixel: in a repeating
            // pattern (stripes, a fence, tiles) many offsets match equally well, and the larger area's motion is
            // the right one. Within half a pixel the refinement is free.
            float c = PatchCost(uv, f, cur) + 0.01 + 0.004 * stepPx + PULL * max(length((f - lead) / texel) - 0.5, 0.0);
            if (c < bestC) { bestC = c; best = f; }
        }
        stepPx *= 0.5;
    }
    // Confidence from the match itself: full below a mean difference of 0.04 per tap, none from 0.10.
    float conf = saturate((0.10 - bestC / 9.0) / 0.06);
    return float4(best, conf, 1.0);
}

// The motion evened out for the whole picture (ta = the 1/8 level's vectors, the target is the 1/32 level's size):
// the mean of all the vectors within about a block and a half of this one (a 7x7 grid of taps, soft weights), leaving out the places
// where nothing was measured. The motion then changes only gradually across the screen -- the camera's motion, with
// its depth -- so nothing can tear or wobble; what moves on its own is carried with its surroundings.
float4 PSWarpSmooth(float4 pos : SV_Position) : SV_Target
{
    float2 sum = float2(0.0, 0.0);
    float wsum = 0.0;
    for (int y = -3; y <= 3; y++)
    {
        for (int x = -3; x <= 3; x++)
        {
            float4 n = ta.SampleLevel(linClamp, (pos.xy + 0.5 * float2(x, y)) * texel, 0);
            float g = exp(-float(x * x + y * y) / 6.0);
            sum += n.xy * g;                   // places with nothing measured hold 0, 0 and w 0
            wsum += n.w * g;
        }
    }
    if (wsum <= 0.02) return float4(0.0, 0.0, 1.0, 0.0);
    // Once more, counting each vector by how well it agrees with that mean (within about two pixels of the 1/8
    // level): a few wrong matches, or something small moving on its own, then don't pull the whole area off.
    float2 m = sum / wsum;
    float2 sum2 = float2(0.0, 0.0);
    float wsum2 = 0.0;
    for (int y2 = -3; y2 <= 3; y2++)
    {
        for (int x2 = -3; x2 <= 3; x2++)
        {
            float4 n = ta.SampleLevel(linClamp, (pos.xy + 0.5 * float2(x2, y2)) * texel, 0);
            if (n.w > 0.001)
            {
                float2 d = (n.xy / n.w - m) / (texel2 * 2.0);
                float g = exp(-float(x2 * x2 + y2 * y2) / 6.0) * exp(-dot(d, d) * 0.5) * n.w;
                sum2 += (n.xy / n.w) * g;
                wsum2 += g;
            }
        }
    }
    return wsum2 > 1e-4 ? float4(sum2 / wsum2, 1.0, 1.0) : float4(m, 1.0, 1.0);
}

// ---- The game's own motion (test): where the game hands its upscaler motion vectors and depth (Streamline / DLSS),
// they are turned into the same kind of motion texture (at the game's vector size, at most half the game frame), at the game's Present, on the
// game's queue (dx/bfi_presenter.cpp, namespace gmo). xy = how far the picture at this point moved from the frame
// before to this one (uv), z = 1 where the game's vector is valid, w = how near the point is (0 far .. 1 near).
cbuffer G : register(b1)
{
    float2 gOutTexel;     // 1 / size of the motion texture written
    float2 gMvScale;      // the game's motion-vector scale (Streamline's mvecScale: to uv)
    float4 gMvMap;        // screen uv -> motion-vector texel: xy scale, zw offset (texels)
    float4 gDepthMap;     // screen uv -> depth texel
    float  gMvInvalid;    // the value the game writes where a vector is not valid
    uint   gFlags;        // 1 depth reversed (near = 1), 2 depth available, 4 the HUD mask (t7) is valid, 8 the frame is linear (scRGB),
                          // 16 dilate the vectors (the nearest point's of 3x3)
    float  gPerRendered;  // shown frames per rendered one (frame generation; 1 without): gMvScale * this = a rendered frame's motion
    float  gPrevOk;       // 1 = t11 holds the vectors of the rendered frame before
    float4 gHMap;         // screen uv -> the game's HUD-less picture's texel
    float  gHudThr;       // HUD mask: a difference above this is HUD
    uint   gHudMode;      // how the frame and the HUD-less picture are compared: 0 as they are (the same encoding);
                          // 1 the frame is HDR10 (PQ, BT.2020) and the picture linear scRGB (BT.709); 2 the frame is
                          // sRGB-encoded and the picture linear
    float2 gPad2;
};
Texture2D<float2> gMv    : register(t5);
Texture2D<float>  gDepth : register(t6);
Texture2D<float>  gHud   : register(t7);   // HUD mask (1 = HUD), the size of the motion texture
Texture2D<float4> hFinal : register(t8);   // HUD mask pass: the game's finished frame
Texture2D<float4> hLess  : register(t9);   // and its HUD-less picture
Texture2D<float>  hPrev  : register(t10);  // and the raw difference found at the Present before
Texture2D<float2> gMvPrev : register(t11); // the vectors of the rendered frame before (same size and format)

float4 PSWarpGame(float4 pos : SV_Position) : SV_Target
{
    // One vector per texel, from the game's vector at its centre (the texture is the size of the game's vectors, or
    // half the frame: no spreading of one vector over its neighbours, which bent thin things).
    float2 uv = pos.xy * gOutTexel;
    // HUD: no motion, and marked (w = -1) so the presenter neither moves it nor takes a moved pixel from it
    if ((gFlags & 4) != 0 && gHud.Load(int3(int2(pos.xy), 0)) > 0.5) return float4(0.0, 0.0, 0.0, -1.0);
    uint mw, mh, dw, dh;
    gMv.GetDimensions(mw, mh);
    gDepth.GetDimensions(dw, dh);
    int2 mq = clamp(int2(uv * gMvMap.xy + gMvMap.zw), int2(0, 0), int2(mw, mh) - 1);
    float2 m = gMv.Load(int3(mq, 0));
    bool mOk = !(m.x == gMvInvalid || m.y == gMvInvalid || any(isnan(m)));
    float nearv = 0.0;
    if ((gFlags & 2) != 0)
    {
        int2 dq = clamp(int2(uv * gDepthMap.xy + gDepthMap.zw), int2(0, 0), int2(dw, dh) - 1);
        float d = gDepth.Load(int3(dq, 0));
        nearv = (gFlags & 1) != 0 ? d : 1.0 - d;
        // Dilation (FrameWarpGameDilate, 1.0.48): the vector of the nearest point of the 3x3 depth texels around this one,
        // as DLSS and NVIDIA's Reflex 2 capture (cuda_capture_mv_dilate_kernel) do. The game's vectors are at its render
        // size, so along the edge of something in front the frame's pixels took the background's vector: the edge stayed
        // behind (doubled outlines, a smear on filled flashes). Now the edge moves with what it belongs to.
        if ((gFlags & 16) != 0 && mOk)
        {
            const float2 dt = 1.0 / gDepthMap.xy;          // one depth texel, in screen uv
            [unroll] for (int y = -1; y <= 1; y++)
                [unroll] for (int x = -1; x <= 1; x++)
                {
                    if (x == 0 && y == 0) continue;
                    const float2 nu = uv + float2(x, y) * dt;
                    const int2 nd = clamp(int2(nu * gDepthMap.xy + gDepthMap.zw), int2(0, 0), int2(dw, dh) - 1);
                    const float dn = gDepth.Load(int3(nd, 0));
                    const float nn = (gFlags & 1) != 0 ? dn : 1.0 - dn;
                    if (nn <= nearv) continue;
                    const int2 nm = clamp(int2(nu * gMvMap.xy + gMvMap.zw), int2(0, 0), int2(mw, mh) - 1);
                    const float2 mn = gMv.Load(int3(nm, 0));
                    if (mn.x == gMvInvalid || mn.y == gMvInvalid || any(isnan(mn))) continue;
                    nearv = nn; m = mn;
                }
        }
    }
    if (!mOk) return float4(0.0, 0.0, 0.0, 0.0);
    // How much the motion kept up (z = 1 + k): this point's motion against its motion one rendered frame before, where it
    // was then; slower now -> k < 1, and a move past the rendered frame is scaled by it (WarpShiftGame). Both under half
    // a pixel: still, k = 1.
    float k = 1.0;
    if (gPrevOk > 0.5)
    {
        const float2 cur = m * gMvScale * gPerRendered;   // uv, a whole rendered frame
        int2 pq = clamp(int2((uv + cur) * gMvMap.xy + gMvMap.zw), int2(0, 0), int2(mw, mh) - 1);
        float2 mp = gMvPrev.Load(int3(pq, 0));
        if (!(mp.x == gMvInvalid || mp.y == gMvInvalid || any(isnan(mp))))
        {
            const float2 px = 1.0 / gOutTexel;
            const float a = length(cur * px), b = length(mp * gMvScale * gPerRendered * px);
            if (b > 0.5) k = saturate(a / b);
        }
    }
    // Streamline: the pixel was at uv + m * mvecScale in the frame before, so it moved by the opposite.
    return float4(-m * gMvScale, 1.0 + k, saturate(nearv));
}

// HUD mask (where the game hands its upscaler a HUD-less picture of the frame, e.g. 007 First Light): what differs
// between the finished frame and the HUD-less picture is HUD. Nine taps over each texel's footprint. Only what
// differed at this Present and the one before counts (raw AND previous raw): with frame generation every other shown
// frame is generated, and a generated frame differs from the rendered frame's HUD-less picture wherever things move
// -- the rendered frame next to it doesn't, so the AND keeps only the HUD.
struct HudOut { float raw : SV_Target0; float hud : SV_Target1; };
// gHudMode 3 (1.0.47): against DLSS's output -- the scene before post-processing and the HUD -- by structure, not value:
// tone mapping, grading, bloom and vignette change the values but keep the scene's local structure (a monotone change);
// the HUD adds structure the scene doesn't have. Over a 6x6 window of the frame around the texel: the frame's
// brightness (its code values; log for a linear frame) against the scene's (log) -- where the frame has structure and
// either the scene is flat there or the two don't go together (correlation under 0.4), it is HUD.
float HudLumF(float3 a) { float y = dot(a, float3(0.2126, 0.7152, 0.0722)); return (gFlags & 8) != 0 ? log2(max(y, 1e-4)) * 0.125 : y; }
float HudLumS(float3 b) { return log2(max(dot(max(b, 0.0), float3(0.2126, 0.7152, 0.0722)), 1e-4)) * 0.125; }
float HudByStructure(float2 uv, uint fw, uint fh, uint lw, uint lh)
{
    float sf = 0.0, ss = 0.0, sff = 0.0, sss = 0.0, sfs = 0.0;
    const float2 fpx = 1.0 / float2(fw, fh);
    [unroll] for (int y = 0; y < 6; y++)
        [unroll] for (int x = 0; x < 6; x++)
        {
            float2 fu = uv + (float2(x, y) - 2.5) * fpx;
            int2 fq = clamp(int2(fu * float2(fw, fh)), int2(0, 0), int2(fw, fh) - 1);
            int2 lq = clamp(int2(fu * gHMap.xy + gHMap.zw), int2(0, 0), int2(lw, lh) - 1);
            float f = HudLumF(hFinal.Load(int3(fq, 0)).rgb), l = HudLumS(hLess.Load(int3(lq, 0)).rgb);
            sf += f; ss += l; sff += f * f; sss += l * l; sfs += f * l;
        }
    const float n = 36.0;
    const float mf = sf / n, ms = ss / n;
    const float vf = max(sff / n - mf * mf, 0.0), vs = max(sss / n - ms * ms, 0.0), cv = sfs / n - mf * ms;
    const float df = sqrt(vf), ds = sqrt(vs);
    if (df < 0.015) return 0.0;                          // nothing to see in the frame here
    if (ds < 0.01) return 1.0;                           // structure in the frame over a flat scene
    return cv / (df * ds) < 0.4 ? 1.0 : 0.0;             // structure that doesn't follow the scene's
}

HudOut PSWarpHud(float4 pos : SV_Position)
{
    float2 uv = pos.xy * gOutTexel;
    uint fw, fh, lw, lh;
    hFinal.GetDimensions(fw, fh);
    hLess.GetDimensions(lw, lh);
    if (gHudMode == 3)
    {
        HudOut s;
        s.raw = HudByStructure(uv, fw, fh, lw, lh);
        s.hud = (s.raw > 0.5 && hPrev.Load(int3(int2(pos.xy), 0)) > 0.5) ? 1.0 : 0.0;
        return s;
    }
    float d = 0.0;
    [unroll] for (int y = 0; y < 3; y++)
    {
        [unroll] for (int x = 0; x < 3; x++)
        {
            float2 fu = uv + ((float2(x, y) + 0.5) / 3.0 - 0.5) * gOutTexel;
            int2 fq = clamp(int2(fu * float2(fw, fh)), int2(0, 0), int2(fw, fh) - 1);
            int2 lq = clamp(int2(fu * gHMap.xy + gHMap.zw), int2(0, 0), int2(lw, lh) - 1);
            float3 a = hFinal.Load(int3(fq, 0)).rgb, b = hLess.Load(int3(lq, 0)).rgb;
            bool rel = (gFlags & 8) != 0;
            if (gHudMode == 1)
            {
                // PQ -> nits -> scRGB (80-nit units), BT.2020 -> BT.709
                float3 p = pow(saturate(a), 1.0 / 78.84375);
                float3 n = pow(max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), 1.0 / 0.1593017578125) * (10000.0 / 80.0);
                a = float3(dot(n, float3(1.6605, -0.5876, -0.0728)), dot(n, float3(-0.1246, 1.1329, -0.0083)), dot(n, float3(-0.0182, -0.1006, 1.1187)));
                rel = true;
            }
            else if (gHudMode == 2)
            {
                a = saturate(a);
                a = lerp(a / 12.92, pow((a + 0.055) / 1.055, 2.4), step(0.04045, a));
                rel = true;
            }
            float3 e = abs(a - b);
            float m = max(max(e.r, e.g), e.b);
            if (rel) m /= 0.2 + max(max(max(abs(a.r), abs(a.g)), abs(a.b)), max(max(abs(b.r), abs(b.g)), abs(b.b)));   // linear: relative
            d = max(d, m);
        }
    }
    HudOut o;
    o.raw = d > gHudThr ? 1.0 : 0.0;
    o.hud = (o.raw > 0.5 && hPrev.Load(int3(int2(pos.xy), 0)) > 0.5) ? 1.0 : 0.0;
    return o;
}
)HLSL";

static const char *const kWarpEntry[] = { "PSWarpLuma", "PSWarpDown", "PSWarpFlow", "PSWarpSmooth", "PSWarpGame", "PSWarpHud" };
static const int kWarpShN = 6;

struct WarpConstants
{
    float    texelX, texelY, texel2X, texel2Y;
    uint32_t mode;
};

struct WarpGpu
{
    static const int kLevels = 5;                       // 1/4, 1/8, 1/16, 1/32, 1/64 of the game frame
    ID3D12Resource       *luma[2][kLevels] = {};        // brightness pyramids of the newest frame shown and the one before
    ID3D12Resource       *flow[kLevels] = {};           // motion, per level
    ID3D12Resource       *flowOut[kLevels] = {};        // the last level's motion evened out, one for each level that can
                                                        // be the last: what the per-refresh shader reads
    ID3D12PipelineState  *psoLuma = nullptr, *psoDown = nullptr, *psoFlow = nullptr, *psoSmooth = nullptr;
    ID3D12Device         *device = nullptr;             // the presenter's (not owned)
    ID3D12DescriptorHeap *rtvHeap = nullptr;
    ID3D12DescriptorHeap *srvHeap = nullptr;            // the presenter's (not owned)
    UINT  w[kLevels] = {}, h[kLevels] = {};
    UINT  srcW = 0, srcH = 0;
    UINT  srvBase = 0, srvStride = 0, rtvStride = 0;
    int   cur = 0;                                      // which pyramid holds the newest frame
    UINT64 id[2] = {};                                  // the game frame in each pyramid (0 = none)
    double t[2] = {};                                   // and when the game presented it
    UINT64 flowNew = 0, flowOld = 0;                    // the pair of frames the motion texture was measured between
    bool  ok = false;

    static const UINT kSrvCount = 4 * kLevels;      // descriptors it takes in the presenter's SRV heap

    static D3D12_RESOURCE_BARRIER Tr(ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
    {
        D3D12_RESOURCE_BARRIER br = {};
        br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        br.Transition.pResource   = r;
        br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        br.Transition.StateBefore = a;
        br.Transition.StateAfter  = b;
        return br;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Srv(UINT i) const
    {
        D3D12_GPU_DESCRIPTOR_HANDLE hnd = srvHeap->GetGPUDescriptorHandleForHeapStart();
        hnd.ptr += (UINT64)(srvBase + i) * srvStride;
        return hnd;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE LumaSrv(int p, int l) const { return Srv((UINT)(p * kLevels + l)); }
    D3D12_GPU_DESCRIPTOR_HANDLE FlowSrvAt(int l) const { return Srv((UINT)(2 * kLevels + l)); }
    // The motion texture the per-refresh shader reads.
    // Whole-picture motion: measured down to 1/8 of the frame, averaged widely at 1/32. (Test builds t1-t2 also had
    // motion per 8x8 or 4x4 block; it tore where things moved differently, so it was dropped.)
    static int Last() { return 1; }                     // the level the motion is refined down to (1/8)
    static int Out() { return 3; }                      // the size of the texture the per-refresh shader reads (1/32)
    D3D12_GPU_DESCRIPTOR_HANDLE FlowSrv() const { return Srv((UINT)(3 * kLevels + Out())); }
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(UINT i) const
    {
        D3D12_CPU_DESCRIPTOR_HANDLE hnd = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        hnd.ptr += (SIZE_T)i * rtvStride;
        return hnd;
    }

    // psBlobs: PSWarpLuma, PSWarpDown, PSWarpFlow, PSWarpSmooth. heap/base: kSrvCount free descriptors in the presenter's SRV heap.
    bool Init(ID3D12Device *dev, ID3D12RootSignature *rootSig, ID3DBlob *vs, ID3DBlob *const *psBlobs,
              ID3D12DescriptorHeap *heap, UINT base, UINT frameW, UINT frameH)
    {
        ok = false;
        if (!dev || !rootSig || !vs || !psBlobs[0] || !psBlobs[1] || !psBlobs[2] || !psBlobs[3] || !heap || frameW < 64 || frameH < 64) return false;
        device = dev;
        srvHeap = heap; srvBase = base; srcW = frameW; srcH = frameH;
        srvStride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        rtvStride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        w[0] = (frameW + 3) / 4; h[0] = (frameH + 3) / 4;
        for (int l = 1; l < kLevels; l++) { w[l] = (w[l - 1] + 1) / 2; h[l] = (h[l - 1] + 1) / 2; }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = rootSig;
        pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.SampleDesc.Count = 1;
        bool m = true;
        pd.RTVFormats[0] = DXGI_FORMAT_R16_FLOAT;
        pd.PS = { psBlobs[0]->GetBufferPointer(), psBlobs[0]->GetBufferSize() };
        m = m && SUCCEEDED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&psoLuma)));
        pd.PS = { psBlobs[1]->GetBufferPointer(), psBlobs[1]->GetBufferSize() };
        m = m && SUCCEEDED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&psoDown)));
        pd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        pd.PS = { psBlobs[2]->GetBufferPointer(), psBlobs[2]->GetBufferSize() };
        m = m && SUCCEEDED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&psoFlow)));
        pd.PS = { psBlobs[3]->GetBufferPointer(), psBlobs[3]->GetBufferSize() };
        m = m && SUCCEEDED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&psoSmooth)));

        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 4 * kLevels;
        m = m && SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap)));

        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        for (int i = 0; m && i < 3 * kLevels; i++)
        {
            const int l = i % kLevels;
            const bool isFlow = i >= 2 * kLevels;
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = w[l]; rd.Height = h[l]; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
            rd.Format = isFlow ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R16_FLOAT;
            rd.SampleDesc.Count = 1;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE cv = {};
            cv.Format = rd.Format;
            ID3D12Resource **slot = isFlow ? &flow[l] : &luma[i / kLevels][l];
            m = SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                       &cv, IID_PPV_ARGS(slot)));
            if (!m) break;
            (*slot)->SetName(isFlow ? L"BFI frame warp motion" : L"BFI frame warp brightness");
            dev->CreateRenderTargetView(*slot, nullptr, Rtv((UINT)i));
            sv.Format = rd.Format;
            D3D12_CPU_DESCRIPTOR_HANDLE sh = srvHeap->GetCPUDescriptorHandleForHeapStart();
            sh.ptr += (SIZE_T)(srvBase + (UINT)i) * srvStride;
            dev->CreateShaderResourceView(*slot, &sv, sh);
        }
        for (int l = 0; m && l < kLevels - 1; l++) m = MakeOut(l);
        if (!m) { Release(); return false; }
        cur = 0; id[0] = id[1] = 0; t[0] = t[1] = 0.0; flowNew = flowOld = 0;
        ok = true;
        return true;
    }

    // The evened-out motion texture for `level` as the last one.
    bool MakeOut(int level)
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w[level]; rd.Height = h[level]; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv = {};
        cv.Format = rd.Format;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                   &cv, IID_PPV_ARGS(&flowOut[level])))) return false;
        flowOut[level]->SetName(L"BFI frame warp motion (evened out)");
        device->CreateRenderTargetView(flowOut[level], nullptr, Rtv((UINT)(3 * kLevels + level)));
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = rd.Format;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE sh = srvHeap->GetCPUDescriptorHandleForHeapStart();
        sh.ptr += (SIZE_T)(srvBase + 3 * kLevels + (UINT)level) * srvStride;
        device->CreateShaderResourceView(flowOut[level], &sv, sh);
        return true;
    }

    void Release()
    {
        for (int i = 0; i < kLevels; i++) if (flowOut[i]) { flowOut[i]->Release(); flowOut[i] = nullptr; }
        if (psoSmooth) { psoSmooth->Release(); psoSmooth = nullptr; }
        for (int p = 0; p < 2; p++)
            for (int l = 0; l < kLevels; l++) if (luma[p][l]) { luma[p][l]->Release(); luma[p][l] = nullptr; }
        for (int l = 0; l < kLevels; l++) if (flow[l]) { flow[l]->Release(); flow[l] = nullptr; }
        if (psoLuma) { psoLuma->Release(); psoLuma = nullptr; }
        if (psoDown) { psoDown->Release(); psoDown = nullptr; }
        if (psoFlow) { psoFlow->Release(); psoFlow = nullptr; }
        if (rtvHeap) { rtvHeap->Release(); rtvHeap = nullptr; }
        ok = false;
    }

    // Forget the frames seen (frame warp switched off and on again, a gap in the frames).
    void Forget() { id[0] = id[1] = 0; flowNew = flowOld = 0; }

    // The frame the motion of `frameId` would be measured from, if that frame were shown now: 0 = none usable.
    // (The frame before it must be the one shown just before, at most 4 game frames and a quarter second older.)
    UINT64 PairFor(UINT64 frameId, double frameT) const
    {
        const UINT64 oldId = frameId == id[cur] ? id[1 - cur] : id[cur];
        const double oldT  = frameId == id[cur] ? t[1 - cur] : t[cur];
        if (!ok || !oldId || frameId <= oldId || frameId - oldId > 4) return 0;
        if (frameT - oldT > 0.25 || frameT < oldT) return 0;
        return oldId;
    }

    void Pass(ID3D12GraphicsCommandList *list, ID3D12PipelineState *pso, ID3D12Resource *target, UINT rtvIndex,
              UINT tw, UINT th, const WarpConstants &wc)
    {
        D3D12_RESOURCE_BARRIER b = Tr(target, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(1, &b);
        list->SetPipelineState(pso);
        list->SetGraphicsRoot32BitConstants(0, sizeof(WarpConstants) / 4, &wc, 0);
        D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)tw, (float)th, 0.0f, 1.0f };
        D3D12_RECT sc = { 0, 0, (LONG)tw, (LONG)th };
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &sc);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = Rtv(rtvIndex);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->DrawInstanced(3, 1, 0, 0);
        b = Tr(target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &b);
    }

    // A new frame is about to be shown for the first time: its brightness pyramid (the one before is kept).
    // frameSrv: the frame (in a shader-resource state); linear: 1 for an scRGB frame. The caller has set the
    // descriptor heap, root signature and topology, and sets its own pipeline state, constants and t0 again after.
    void RecordLuma(ID3D12GraphicsCommandList *list, D3D12_GPU_DESCRIPTOR_HANDLE frameSrv, UINT64 frameId, double frameT, bool linear)
    {
        cur = 1 - cur;
        id[cur] = frameId; t[cur] = frameT;
        WarpConstants wc = {};
        wc.texelX = 1.0f / (float)w[0]; wc.texelY = 1.0f / (float)h[0];
        wc.texel2X = 1.0f / (float)srcW; wc.texel2Y = 1.0f / (float)srcH;
        wc.mode = linear ? 1u : 0u;
        list->SetGraphicsRootDescriptorTable(1, frameSrv);
        Pass(list, psoLuma, luma[cur][0], (UINT)(cur * kLevels), w[0], h[0], wc);
        for (int l = 1; l < kLevels; l++)
        {
            wc.texelX = 1.0f / (float)w[l]; wc.texelY = 1.0f / (float)h[l];
            list->SetGraphicsRootDescriptorTable(1, LumaSrv(cur, l - 1));
            Pass(list, psoDown, luma[cur][l], (UINT)(cur * kLevels + l), w[l], h[l], wc);
        }
    }

    // The motion from the frame before to the newest, coarse to fine. Only when a frame is shown moved, and once per
    // pair of frames. Sets t0-t2; the caller sets its own again after.
    void RecordFlow(ID3D12GraphicsCommandList *list)
    {
        const int top = kLevels - 1;
        const int stop = Last();
        for (int l = top; l >= stop; l--)
        {
            WarpConstants wc = {};
            wc.texelX = 1.0f / (float)w[l]; wc.texelY = 1.0f / (float)h[l];
            wc.mode = (l < top ? 1u : 0u) | (l == stop ? 2u : 0u);
            if (l < top) { wc.texel2X = 1.0f / (float)w[l + 1]; wc.texel2Y = 1.0f / (float)h[l + 1]; }
            list->SetGraphicsRootDescriptorTable(1, LumaSrv(cur, l));
            list->SetGraphicsRootDescriptorTable(2, LumaSrv(1 - cur, l));
            list->SetGraphicsRootDescriptorTable(3, l < top ? FlowSrvAt(l + 1) : LumaSrv(cur, l));   // unused at the top level
            Pass(list, psoFlow, flow[l], (UINT)(2 * kLevels + l), w[l], h[l], wc);
        }
        {
            WarpConstants wc = {};
            const int o = Out();
            wc.texelX = 1.0f / (float)w[o]; wc.texelY = 1.0f / (float)h[o];
            wc.texel2X = 1.0f / (float)w[stop]; wc.texel2Y = 1.0f / (float)h[stop];
            wc.mode = 1u;
            list->SetGraphicsRootDescriptorTable(1, FlowSrvAt(stop));
            Pass(list, psoSmooth, flowOut[o], (UINT)(3 * kLevels + o), w[o], h[o], wc);
        }
        flowNew = id[cur]; flowOld = id[1 - cur];
    }
};
