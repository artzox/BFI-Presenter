// CRT hand-off, Vulkan (CrtStandaloneHandoff, 1.0.54 test; bfi_crt_vk.h). CRT-Standalone's own pattern stage and the
// passes after it (crt_gen.hlsl: dx/bfi_crt_gen.h's kCrtHlsl, written out by build_mingw.sh), as compute passes,
// compiled to SPIR-V when the layer is built (glslang, HLSL in). The DX add-on compiles them with the shader's
// switches when they change; the layer has no compiler with it, so here every switch is compiled in and one that is off
// is given its off value instead (bfi_crt_vk.h: a strength of 0 -- the shader's own test then leaves the picture as it is).
// ENABLE_BEAM_MODULATION changes the code rather than a strength: two builds of the pattern (BEAM 0/1).
#define PIPELINE 0
#define ENABLE_MASK 1
#define ENABLE_BEAM_MODULATION BEAM
#define ENABLE_HALATION 1
#define ENABLE_GLOW 1
#define ENABLE_VIGNETTE 1
#define ENABLE_CORNER_ROUND 1
#define ENABLE_BURNIN_PHASE 1
#define ENABLE_BURNIN_ORBIT 1
#define ENABLE_INTERLACE 1
#define ENABLE_SCANLINE_SS 0
#define ENABLE_DECAY 0
#define ENABLE_PREBLUR 1
#define ENABLE_GEOMETRY 0
#define SCANLINE_REFERENCE_HEIGHT 0
#define ENABLE_SHARPEN 1
#define ENABLE_SCANLINE_SOFTEN 1
#define ENABLE_GRAIN 1
#define ENABLE_NOISE_FLOOR 1
#define ENABLE_GAMUT_EXPAND 1
#define CRT_POST 1
#define CrtPixelSize (1.0 / float2(BUFFER_WIDTH, BUFFER_HEIGHT))
[[vk::binding(0, 0)]] Texture2D<float4> inPic;       // the pattern: the frame as shown (crtwarp.comp)
[[vk::binding(1, 0)]] Texture2D<float4> crtHalo;     // CRT-Standalone's halation, glow and wide glow (this frame's)
[[vk::binding(2, 0)]] Texture2D<float4> crtGlow;
[[vk::binding(3, 0)]] Texture2D<float4> crtGlowWide;
[[vk::binding(4, 0)]] Texture2D<float4> crtPrev;     // the passes after it: the pass before
[[vk::binding(5, 0)]] Texture2D<float4> crtGrainRaw; // and grain's own texture
[[vk::binding(6, 0)]] SamplerState linClamp;
[[vk::binding(7, 0)]] RWTexture2D<float4> outPic;   // rgba8, as ReShade's back buffer (build_mingw.sh sets it in the SPIR-V: glslang declares Rgba32f)
[[vk::binding(9, 0)]] Texture2D<uint> srcShift;      // where each pixel came from: the offset, two halves (crtwarp.comp)
float4 CrtBB(float2 uv) { return inPic.SampleLevel(linClamp, uv, 0); }
float4 CrtHalo(float2 uv) { return crtHalo.SampleLevel(linClamp, uv, 0); }
float4 CrtGlow(float2 uv) { return crtGlow.SampleLevel(linClamp, uv, 0); }
float4 CrtGlowWide(float2 uv) { return crtGlowWide.SampleLevel(linClamp, uv, 0); }
static float4 crtFused = 0.0;
static bool crtFusing = false;
float4 CrtPrev(float2 uv) { if (crtFusing) return crtFused; return crtPrev.SampleLevel(linClamp, uv, 0); }
float4 CrtGrainRaw(float2 uv) { return crtGrainRaw.SampleLevel(linClamp, uv, 0); }
#include "crt_gen.hlsl"

bool InFrame(uint2 id) { uint w, h; outPic.GetDimensions(w, h); return id.x < w && id.y < h; }

// the mask and scanlines at the screen's pixel; the picture, halation and glow from where it came from (moved)
[numthreads(8, 8, 1)]
void CSPattern(uint3 id : SV_DispatchThreadID)
{
    if (!InFrame(id.xy)) return;
    const float2 fc = float2(id.xy) + 0.5;
    const float3 c = inPic.Load(int3(id.xy, 0)).rgb;
    const uint sh = srcShift.Load(int3(id.xy, 0));
    const float2 from = fc + float2(f16tof32(sh & 0xFFFFu), f16tof32(sh >> 16));
    outPic[id.xy] = float4(CrtPattern(c, fc, from * CrtPixelSize), 1.0);
}
[numthreads(8, 8, 1)]
void CSSharpen(uint3 id : SV_DispatchThreadID) { if (InFrame(id.xy)) outPic[id.xy] = PSCrtSharpen(float4(float2(id.xy) + 0.5, 0.0, 1.0)); }
[numthreads(8, 8, 1)]
void CSSoften(uint3 id : SV_DispatchThreadID) { if (InFrame(id.xy)) outPic[id.xy] = PSCrtSoften(float4(float2(id.xy) + 0.5, 0.0, 1.0)); }
[numthreads(8, 8, 1)]
void CSGrainRaw(uint3 id : SV_DispatchThreadID) { if (InFrame(id.xy)) outPic[id.xy] = PSCrtGrainRaw(float4(float2(id.xy) + 0.5, 0.0, 1.0)); }
// grain's second half, the noise floor, gamut and the corners in one pass (as the DX add-on's PSCrtTail): all read only
// their own pixel of the pass before; always drawn (with all of them off it passes the picture on into the final texture)
[numthreads(8, 8, 1)]
void CSTail(uint3 id : SV_DispatchThreadID)
{
    if (!InFrame(id.xy)) return;
    const float4 pos = float4(float2(id.xy) + 0.5, 0.0, 1.0);
    const float2 uv = pos.xy * CrtPixelSize;
    float4 o;
    crt_grain_diffuse_PS(pos, uv, o);
    crtFusing = true;
    crtFused = o; crt_noise_floor_PS(pos, uv, o);
    crtFused = o; crt_gamut_expand_PS(pos, uv, o);
    crtFused = o; crt_corner_round_PS(pos, uv, o);
    outPic[id.xy] = o;
}
