// BFI per-pixel work, shared by bfi.frag (graphics queue) and bfi.comp (realtime compute queue).

// Split comparison: the newest game frame, for the side without BFI (set 1).
layout(set = 1, binding = 0) uniform sampler2D srcNew;

// Frame warp (set 2, see warp.comp and dx/bfi_warp.h): the measured motion, the game frame shown before this one, and
// this frame again with a linear sampler (src samples nearest).
layout(set = 2, binding = 0) uniform sampler2D flowTex;   // xy = how far the picture moved since the frame before (uv), z = confidence
layout(set = 2, binding = 1) uniform sampler2D srcPrev;
layout(set = 2, binding = 2) uniform sampler2D srcLin;
layout(set = 2, binding = 3) uniform sampler2D gameFlow;   // the game's own motion for this frame (gameflow.comp), half size
// FrameWarpStillBeforeEffects (1.0.48, as DX; flags & 16384): the frames from before ReShade's effects, where what stands
// still is found (a CRT shader's blur and glow over a HUD made it look as if it moved). Otherwise the stage frames.
layout(set = 2, binding = 4) uniform sampler2D preCur;
layout(set = 2, binding = 5) uniform sampler2D prePrev;
// 1.0.49 (as DX): and, while ReShade's window is open (flags & 32768), where it stands still in the finished frames too
// -- ReShade's window, drawn after the effects, is only in those (it was moved with the scene in 1.0.48).
bool gStillPre = false;
vec3 StillCur(ivec2 q) { return gStillPre ? texelFetch(preCur, q, 0).rgb : texelFetch(src, q, 0).rgb; }
vec3 StillPrev(ivec2 q) { return gStillPre ? texelFetch(prePrev, q, 0).rgb : texelFetch(srcPrev, q, 0).rgb; }
vec3 StillPrevAt(vec2 uv) { return gStillPre ? textureLod(prePrev, uv, 0.0).rgb : textureLod(srcPrev, uv, 0.0).rgb; }

// The game's own motion (as dx/bfi_presenter.cpp, WarpShiftGame): the place the output pixel comes from is found in up
// to three steps along the motion, and each step checked -- does the point found really move onto this pixel? Of the
// steps that do, the nearest point wins; where none does, the background's motion (the farthest point) fills it.
// With frame generation, the vectors are read warpLag shown frames further along each point's motion.
vec4 GameFlow(vec2 uv)
{
    ivec2 s = textureSize(gameFlow, 0);
    return texelFetch(gameFlow, clamp(ivec2(uv * vec2(s)), ivec2(0), s - 1), 0);
}
bool gFromPrev = false;   // interpolation (flags & 65536, as DX 1.0.51): this pixel is taken from the frame before (srcPrev)
vec2 WarpShiftGame(vec2 uv)
{
    gFromPrev = false;
    vec2 osz = vec2(textureSize(src, 0));
    float L = pc.warp - pc.warpLag;
    vec2 m[3];
    m[0] = GameFlow(uv).xy;
    m[1] = GameFlow(uv - m[0] * L).xy;
    m[2] = GameFlow(uv - m[1] * L).xy;
    vec2 best = vec2(0.0), back = vec2(0.0), backOwn = vec2(0.0);
    float bestNear = -1.0, backNear = 2.0, bestK = 1.0, backK = 1.0;
    for (int i = 0; i < 3; i++)
    {
        vec4 g = GameFlow(uv - m[i] * L);
        if (g.z < 0.5) continue;
        if (length((g.xy - m[i]) * abs(L) * osz) < 2.0 && g.w > bestNear) { bestNear = g.w; best = m[i]; bestK = clamp(g.z - 1.0, 0.0, 1.0); }
        if (g.w < backNear) { backNear = g.w; back = m[i]; backOwn = g.xy; backK = clamp(g.z - 1.0, 0.0, 1.0); }
    }
    // Interpolated (warp = -(1 - f)): what the newer frame doesn't show here is about to be covered in it -- the frame before
    // still shows it, moved on by f along the background's own motion.
    if ((pc.flags & 65536u) != 0u && bestNear < 0.0 && backNear <= 1.0)
    {
        gFromPrev = true;
        return clamp(backOwn * (1.0 + pc.warp), -0.08, 0.08);
    }
    if (bestNear < 0.0) { best = backNear <= 1.0 ? back : vec2(0.0); bestK = backK; }
    // Up to the rendered frame the motion is known (a generated frame); past it, it's a guess that it carries on: that part
    // eases off as the motion slows (flags & 2048), so a camera coming to a stop isn't pushed past where it stops (as DX ep9).
    float known = clamp(pc.warpLag, 0.0, pc.warp);
    // its strength (FrameWarpGameMotionEaseStrength, 1.0.49): flags bits 24-31, in 64ths (64 = 1, as before 1.0.49)
    float e = float(pc.flags >> 24) / 64.0;
    float k = ((pc.flags & 2048u) != 0u && e > 0.001) ? pow(max(bestK, 0.0), e) : 1.0;
    return clamp(best * (known + (pc.warp - known) * k), -0.08, 0.08);
}

// How far back along its motion the picture shown at uv is taken from, in uv (read where the picture comes from, so a
// moving area's own motion is used); at most 8% of the screen.
vec2 WarpShift(vec2 uv)
{
    if (pc.warpSrc != 0u) return WarpShiftGame(uv);
    vec4 f = textureLod(flowTex, uv, 0.0);
    vec2 d = f.xy * (f.z * pc.warp);
    f = textureLod(flowTex, uv - d, 0.0);
    return clamp(f.xy * (f.z * pc.warp), -0.08, 0.08);
}
// A pixel that is the same in this frame and the one shown before it.
bool Unchanged(ivec2 q, ivec2 size)
{
    if (q.x < 0 || q.y < 0 || q.x >= size.x || q.y >= size.y) return true;
    vec3 a = StillCur(q), b = StillPrev(q);
    vec3 tol = (pc.flags & 1024u) != 0u ? 0.002 * (0.1 + max(abs(a), abs(b))) : vec3(0.0016);
    return all(lessThanEqual(abs(a - b), tol));
}
// Does the pixel stand still on the screen (a HUD, text, a crosshair) rather than move with the scene? (As in DX.)
bool StillIn(ivec2 q, vec2 size)
{
    ivec2 isz = ivec2(size);
    if (!Unchanged(q, isz)) return false;
    // (as DX: no 5-of-8 shortcut since 1.0.46 -- a clear sky stood still with a tree's outline against it; and since
    // 1.0.47 a flat area only where it is exactly the same around it, as a HUD is -- patches of soft clouds were held)
    vec3 a0 = StillCur(q);
    float lc = (pc.flags & 1024u) != 0u ? 0.06 * (0.1 + max(max(a0.r, a0.g), a0.b)) : 0.03;
    int n = 0;
    float con = 0.0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++)
            if (x != 0 || y != 0)
            {
                ivec2 nq = clamp(q + ivec2(x, y), ivec2(0), isz - 1);
                if (Unchanged(q + ivec2(x, y), isz)) n++;
                vec3 e = abs(StillCur(nq) - a0);
                con = max(con, max(max(e.r, e.g), e.b));
            }
    if (n < 1) return false;
    if (con < lc)
        for (int y = -1; y <= 1; y++)
            for (int x = -1; x <= 1; x++)
            {
                ivec2 nq = clamp(q + ivec2(x, y), ivec2(0), isz - 1);
                if (any(notEqual(StillCur(nq), StillPrev(nq)))) return false;
            }
    vec2 c = vec2(q) + 0.5;
    vec4 f = pc.warpSrc != 0u ? textureLod(gameFlow, c / size, 0.0) : textureLod(flowTex, c / size, 0.0);
    vec2 step = f.xy * size * (pc.warpSrc != 0u ? min(f.z, 1.0) * pc.warpPrevN : f.z);
    if (dot(step, step) < 2.25) return true;
    vec3 a = StillCur(q);
    vec3 d = abs(a - StillPrevAt((c - step) / size));
    float lim = (pc.flags & 1024u) != 0u ? 0.04 * (0.1 + max(max(a.r, a.g), a.b)) : 0.04;
    return max(max(d.r, d.g), d.b) > lim;
}
bool Still(ivec2 q, vec2 size)
{
    gStillPre = (pc.flags & 16384u) != 0u;
    bool r = StillIn(q, size);
    gStillPre = false;
    if (r || (pc.flags & 16384u) == 0u || (pc.flags & 32768u) == 0u) return r;
    return StillIn(q, size);   // ReShade's window open (flags & 32768): the finished frames too
}
// The screen-edge strip of a moved flash (FrameWarpEdge; as DX): mirrored at the edge (flags & 4096), or the move
// eased off over a band three moves wide on the side the picture comes in from (flags & 8192).
vec2 EdgeMirror(vec2 p, vec2 size)
{
    p = mix(p, 1.0 - p, lessThan(p, vec2(0.5)));
    return mix(p, 2.0 * size - 1.0 - p, greaterThan(p, size - 0.5));
}
vec2 EdgeFade(vec2 cp, vec2 d, vec2 size)
{
    vec2 dist = mix(size - cp, cp, greaterThan(d, vec2(0.0)));
    vec2 band = 3.0 * abs(d);
    vec2 k = mix(vec2(1.0), clamp(dist / max(band, vec2(0.5)), 0.0, 1.0), greaterThan(band, vec2(0.5)));
    return d * k;
}
// The game's frame moved ahead (Catmull-Rom, 9 bilinear taps). False = show the pixel as rendered.
bool InOsd(vec2 p)
{
    if (pc.osdB == 0u) return false;
    vec2 a = vec2(float(pc.osdA & 0xFFFFu), float(pc.osdA >> 16)), b = vec2(float(pc.osdB & 0xFFFFu), float(pc.osdB >> 16));
    return all(greaterThanEqual(p, a)) && all(lessThan(p, b));
}
bool FetchWarped(vec2 fc, out vec3 c)
{
    c = vec3(0.0);
    vec2 size = vec2(textureSize(src, 0));
    vec2 inv = 1.0 / size;
    vec2 d = WarpShift(fc * inv) * size;
    if ((pc.flags & 8192u) != 0u) d = EdgeFade(fc, d, size);
    if (pc.warpSnapX > 0.0) d.x = round(d.x / pc.warpSnapX) * pc.warpSnapX;
    if (pc.warpSnapY > 0.0) d.y = round(d.y / pc.warpSnapY) * pc.warpSnapY;
    if (dot(d, d) < 0.0004) return false;
    if (InOsd(fc)) return false;                          // the BFI OSD: never moved (as DX 1.0.47)
    if (Still(ivec2(fc), size)) return false;
    vec2 p = fc - d;
    if ((pc.flags & 4096u) != 0u) p = EdgeMirror(p, size);
    if (InOsd(p)) return false;                           // and nothing moved is taken from it
    p = clamp(p, vec2(0.5), size - 0.5);
    if (Still(ivec2(p), size)) return false;
    if (gFromPrev) { c = textureLod(srcPrev, p * inv, 0.0).rgb; return true; }   // interpolation: from the frame before
    vec2 tc = floor(p - 0.5) + 0.5;
    vec2 f = p - tc;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 t0 = (tc - 1.0) * inv, t3 = (tc + 2.0) * inv, t12 = (tc + w2 / w12) * inv;
    c += textureLod(srcLin, vec2(t0.x,  t0.y),  0.0).rgb * (w0.x  * w0.y);
    c += textureLod(srcLin, vec2(t12.x, t0.y),  0.0).rgb * (w12.x * w0.y);
    c += textureLod(srcLin, vec2(t3.x,  t0.y),  0.0).rgb * (w3.x  * w0.y);
    c += textureLod(srcLin, vec2(t0.x,  t12.y), 0.0).rgb * (w0.x  * w12.y);
    c += textureLod(srcLin, vec2(t12.x, t12.y), 0.0).rgb * (w12.x * w12.y);
    c += textureLod(srcLin, vec2(t3.x,  t12.y), 0.0).rgb * (w3.x  * w12.y);
    c += textureLod(srcLin, vec2(t0.x,  t3.y),  0.0).rgb * (w0.x  * w3.y);
    c += textureLod(srcLin, vec2(t12.x, t3.y),  0.0).rgb * (w12.x * w3.y);
    c += textureLod(srcLin, vec2(t3.x,  t3.y),  0.0).rgb * (w3.x  * w3.y);
    return true;
}
// The game's frame at output pixel fc (same size), moved ahead on a filled strobe cycle.
vec3 Fetch(vec2 fc)
{
    if (pc.warp != 0.0)
    {
        vec3 m;
        if (FetchWarped(fc, m)) return m;
    }
    return texelFetch(src, ivec2(fc), 0).rgb;
}

// BFI highlight roll-off: hue-preserving shoulder toward the ceiling P.
vec3 Rolloff(vec3 lin, float g, float P, float k)
{
    float m = max(max(lin.r, lin.g), lin.b);
    vec3 r = lin * g;
    if (g > 1.0 && m > 1e-6)
    {
        float u = m / P;
        float v = g * u;
        k = clamp(k, 0.05, 0.99);
        if (u >= 1.0)
            r = lin;
        else if (v > k)
        {
            float a = (g - k) / (1.0 - k);
            float t = clamp((v - k) / (g - k), 0.0, 1.0);
            float o = k + (1.0 - k) * (1.0 - pow(1.0 - t, a));
            r = lin * (o / u);
        }
    }
    return r;
}

// Variable MPRT brightness budget: front-loaded fill, overflow wraps round.
float MprtScale(uint k, float litGain, float fl, float S)
{
    uint k0 = 0u;
    if (Inverted()) k0 = ((pc.flags & 2u) != 0u) ? (pc.frames / 2u) : 1u;
    uint pos = (k + pc.frames - k0) % pc.frames;
    float fill[8];
    float carry = 0.0;
    for (uint j = 0u; j < 8u; j++)
    {
        fill[j] = 0.0;
        if (j < pc.frames)
        {
            float want = Target((k0 + j) % pc.frames, litGain, fl) + carry;
            fill[j] = min(want, S);
            carry = want - fill[j];
        }
    }
    float s = 0.0;
    for (uint j2 = 0u; j2 < 8u; j2++)
    {
        if (j2 < pc.frames)
        {
            float add = min(carry, S - fill[j2]);
            carry -= add;
            if (j2 == pos) s = fill[j2] + add;
        }
    }
    return s;
}

// One pixel's light in sub-frame j of the cycle.
// LCD gradual saver (after the Blur Busters CRT simulator's slewing LCD Saver): see the DX add-on.
float Overlap(float a, float w, float j, float n)
{
    float o = max(0.0, min(a + w, j + 1.0) - max(a, j));
    o += max(0.0, min(a + w, j + 1.0 + n) - max(a, j + n));
    return o;
}
float GradualGain(uint j, float litGain, float fl, float S)
{
    float E = litGain - fl;
    if (E <= 0.0) return fl;
    float n = float(pc.frames);
    float h = min(E, max(S - fl, 1e-4));
    float w = E / h;
    if (w >= n) { w = n; h = E / n; }
    return fl + h * Overlap(pc.phase, w, float(j), n);
}

vec3 Strobe(vec3 lin, uint j, float litGain)
{
    if ((pc.flags & 16u) != 0u)
    {
        if (pc.method == 0u)
        {
            // Roll the highlights off once for the whole lit image, then share that light out.
            vec3 full = ((pc.flags & 1u) != 0u) ? Rolloff(lin, litGain, pc.ceilLin, pc.knee) : lin * litGain;
            vec3 base = lin * pc.dfloor;
            return base + (full - base) * Overlap(pc.phase, 1.0, float(j), float(pc.frames));
        }
        float mg = max(max(lin.r, lin.g), lin.b);
        float Sg = (mg > 1e-6) ? max(pc.ceilLin / mg, 1.0) : 1e6;
        return lin * GradualGain(j, litGain, pc.dfloor, Sg);
    }
    if (pc.method == 0u)
    {
        float g = Target(j, litGain, pc.dfloor);
        return ((pc.flags & 1u) != 0u) ? Rolloff(lin, g, pc.ceilLin, pc.knee) : lin * g;
    }
    float m = max(max(lin.r, lin.g), lin.b);
    float S = (m > 1e-6) ? max(pc.ceilLin / m, 1.0) : 1e6;
    return lin * MprtScale(j, litGain, pc.dfloor, S);
}

// Phosphor decay: share of a pixel's light that falls in sub-frame s when the
// beam excites it at phase phi (in sub-frames, 0..n) and it fades with time
// constant T (sub-frames). Periodic over the cycle: the shares add up to 1.
float DecayShare(uint s, float phi, float T, float n)
{
    float a = float(s) - phi;
    a = a - n * floor(a / n);
    float norm = 1.0 - exp(-n / T);
    float b = a + 1.0;
    if (b <= n) return (exp(-a / T) - exp(-b / T)) / norm;
    return (exp(-a / T) - exp(-n / T) + 1.0 - exp(-(b - n) / T)) / norm;
}

// The BFI picture for one pixel (fc: its centre, in pixels).
vec4 BfiShade(vec2 fc)
{
    vec3 c = Fetch(fc);
    if (pc.method == 2u) return vec4(pc.outType >= 4u ? Encode(Decode(c)) : c, 1.0);
    vec3 lin = Decode(c);
    float litGain = LitGain();
    vec3 o;
    if (pc.method == 3u)
    {
        // Hitch guard: every refresh at the strobe's average brightness, so
        // pausing and resuming BFI changes motion clarity only, not brightness.
        float a = 0.0;
        for (uint j = 0u; j < 8u; j++) if (j < pc.frames) a += Target(j, litGain, pc.dfloor);
        o = lin * (a / float(pc.frames));
    }
    else if ((pc.flags & 8u) != 0u && pc.decay > 0.0)
    {
        // Rolling scan with phosphor decay (Blur Busters CRT beam simulator
        // style): the beam sweeps the screen once per cycle, reaching row y at
        // phase y * frames; the row then fades, so its light spreads over the
        // following sub-frames. Each pixel still gets one full cycle's light,
        // and there is no seam: the band edge moves with the beam and fades.
        float n = float(pc.frames);
        float phi = (fc.y / float(textureSize(src, 0).y)) * n;
        float T = max(pc.decay, 0.02);
        o = vec3(0.0);
        for (uint j = 0u; j < 8u; j++)
        {
            if (j < pc.frames)
            {
                uint d = (pc.slot + pc.frames - j) % pc.frames;
                float e = DecayShare(d, phi, T, n);
                if (e > 1e-5) o += Strobe(lin, j, litGain) * e;
            }
        }
    }
    else if ((pc.flags & 8u) != 0u)
    {
        // Rolling scan: the screen is cut into one horizontal band per
        // sub-frame, and band b runs b sub-frames behind the top one, so the
        // lit band rolls down the screen and the screen as a whole never goes
        // dark. Each pixel still gets the same light over the cycle; across
        // the seam between two bands it is shared between their sub-frames.
        float H = float(textureSize(src, 0).y);
        float u = (fc.y / H) * float(pc.frames);
        uint band = min(uint(u), pc.frames - 1u);
        float f = u - float(band);
        float hw = max(pc.seam * float(pc.frames) * 0.5, 1e-4);
        o = Strobe(lin, (pc.slot + pc.frames - band) % pc.frames, litGain);
        float w = 0.0; uint nb = band;
        if (f < hw && band > 0u)                        { w = 0.5 * (1.0 - f / hw);         nb = band - 1u; }
        else if (1.0 - f < hw && band + 1u < pc.frames) { w = 0.5 * (1.0 - (1.0 - f) / hw); nb = band + 1u; }
        if (w > 0.0) o = mix(o, Strobe(lin, (pc.slot + pc.frames - nb) % pc.frames, litGain), w);
    }
    else o = Strobe(lin, pc.slot, litGain);
    if (pc.splitMode != 0u && pc.method < 2u)
    {
        // Split comparison (idea: unrefined803, #111): one side shows every refresh at the light the strobe gives over
        // its cycle -- the mean of what each refresh emits, after roll-off and the ceiling -- so both sides match in
        // brightness and differ only in motion clarity and flicker. It shows the newest game frame at every refresh, as a
        // display without BFI would (120 fps at 120 Hz: a new frame each refresh), while the BFI side keeps one frame per
        // strobe cycle.
        float t = pc.splitSeam > 0.0 ? smoothstep(pc.splitX - 0.5 * pc.splitSeam, pc.splitX + 0.5 * pc.splitSeam, fc.x)
                                     : step(pc.splitX, fc.x);
        float w = pc.splitMode == 1u ? 1.0 - t : t;
        if (w > 0.0)
        {
            vec3 a = vec3(0.0);
            vec3 linNew = Decode(texelFetch(srcNew, ivec2(fc.xy), 0).rgb);
            for (uint j = 0u; j < 8u; j++) if (j < pc.frames) a += Strobe(linNew, j, litGain);
            o = mix(o, a / float(pc.frames), w);
        }
    }
    if (pc.soften > 0.0 && pc.method < 2u)
    {
        // LCD phase swap: fade toward the steady average picture (the hitch guard's), which keeps the light
        // of every cycle the same while the strobe's depth goes down and back up.
        float a = 0.0;
        for (uint j = 0u; j < 8u; j++) if (j < pc.frames) a += Target(j, litGain, pc.dfloor);
        o = mix(o, lin * (a / float(pc.frames)), pc.soften);
    }
    return vec4(Encode(o), 1.0);
}
