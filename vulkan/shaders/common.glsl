layout(push_constant) uniform C
{
    uint  outType;    // 0 SDR (sRGB-encoded UNORM), 1 scRGB, 2 HDR10 (PQ), 3 SDR in an _SRGB format (hardware decodes/encodes),
                      // 4 SDR (sRGB-encoded) in, HDR10 out, 5 SDR from an _SRGB format in, HDR10 out
    uint  slot;       // sub-frame index within the cycle
    uint  frames;
    uint  method;     // 0 BFI, 1 Variable MPRT, 2 passthrough, 3 steady (hitch guard)
    float gain;
    float dfloor;
    float knee;
    float ceilLin;    // panel ceiling, linear (1.0 = SDR white / 80 nits)
    uint  flags;      // 1 roll-off, 2 sine, 4 invert, 8 rolling scan
    float sdrScale;   // outType 4/5 (SDR game shown as HDR10): SDR white in 80-nit units
    float sdrKnee;    // highlight expansion starts here (linear, 1.0 = SDR white)
    float sdrPeak;    // SDR white highlights expand to this (80-nit units); <= sdrScale = off
    float seam;       // rolling scan (flags & 8): blended zone between bands, fraction of the screen height
    float decay;      // rolling scan: phosphor decay time in sub-frames (0 = hard bands + seam)
    float soften;     // 0..1: blend toward the steady average picture (LCD phase swap fade)
    float phase;      // LCD gradual saver: start of the lit window within the first sub-frame, 0..1
    uint  store;      // compute path only: 1 swap R and B (B-first swap chain format), 2 sRGB-encode (_SRGB swap chain format)
    float gamut;      // outType 4/5: gamut expansion strength (0 = off)
    float splitX;     // split comparison: the seam, in output pixels
    float splitSeam;  // its soft width in output pixels (0 = hard edge)
    uint  splitMode;  // 0 off, 1 the left side without BFI, 2 the right side
    float rfxRamp;    // SDR to HDR, RenoFX HDR Boost (flags & 256): Ramp Intensity
    float rfxColor;   // its colour blend, 0..1 (0 = brightness only)
    float rfxAplCut;  // its APL Limiter: share of the boost taken away in bright scenes (0 = full boost, 1 = none)
} pc;
layout(set = 0, binding = 0) uniform sampler2D src;

vec3 SrgbToLin(vec3 c) { c = max(c, 0.0); return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c)); }
vec3 LinToSrgb(vec3 l) { l = max(l, 0.0); return mix(l * 12.92, 1.055 * pow(l, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), l)); }
const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
vec3 PqToLin(vec3 e)
{
    vec3 p = pow(clamp(e, 0.0, 1.0), vec3(1.0 / m2));
    return pow(max(p - c1, 0.0) / (c2 - c3 * p), vec3(1.0 / m1)) * (10000.0 / 80.0);
}
vec3 LinToPq(vec3 l)
{
    vec3 p = pow(clamp(max(l, 0.0) * (80.0 / 10000.0), 0.0, 1.0), vec3(m1));
    return pow((c1 + c2 * p) / (1.0 + c3 * p), vec3(m2));
}
// SDR shown as HDR10 (outType 4/5). SDR white at sdrScale, and above the knee the
// brightest channel (flags & 128: the luminance) is stretched toward sdrPeak (hue kept).
// Below the knee the image is untouched; the curve meets the straight part with the
// same slope, so there is no visible seam:  out = W*m + (P - W) * t^2.
vec3 SdrExpand(vec3 l)
{
    l = max(l, 0.0);
    float m = (pc.flags & 128u) != 0u ? dot(l, vec3(0.2126, 0.7152, 0.0722)) : max(max(l.r, l.g), l.b);
    if (pc.sdrPeak <= pc.sdrScale || m <= pc.sdrKnee) return l * pc.sdrScale;
    float t = (min(m, 1.0) - pc.sdrKnee) / max(1.0 - pc.sdrKnee, 1e-4);
    float o = pc.sdrScale * m + (pc.sdrPeak - pc.sdrScale) * t * t;
    return l * (o / m);
}
// Gamut expansion (Pumbo's AdvancedAutoHDR "Extra HDR saturation", after Unreal Engine): bright saturated colours
// move out of BT.709 toward a gamut between P3 and ACES AP1, as if the game had rendered wider colours. The weight
// grows with the colour's distance from grey (in AP1) and its brightness, at a 203-nit paper white whatever SDR white
// is set to, as in AdvancedAutoHDR. Matrices folded from its Color.fxh: AP1 is BT.709 through the D65 -> D60 CAT.
vec3 ExpandGamut(vec3 v, float amount)
{
    const float pw = 203.0 / 80.0;
    vec3 r = v * pw;
    vec3 ap1 = vec3(dot(r, vec3(0.613191478, 0.339512089, 0.047366331)),
                    dot(r, vec3(0.070206904, 0.916335817, 0.013450011)),
                    dot(r, vec3(0.020618871, 0.109567294, 0.869606747)));
    float luma = dot(ap1, vec3(0.2722287168, 0.6740817658, 0.0536895174));
    if (luma <= 0.0) return v;
    vec3 ch = ap1 / luma - 1.0;
    float k = (1.0 - exp2(-4.0 * dot(ch, ch))) * (1.0 - exp2(-4.0 * amount * luma * luma));
    vec3 e = vec3(dot(r, vec3(1.375026308, -0.342531728, -0.079080858)),
                  dot(r, vec3(-0.077049702, 1.092442754, -0.010129544)),
                  dot(r, vec3(-0.020494311, -0.091839135, 1.203626612)));
    return mix(r, e, k) / pw;
}
// flags & 32: SDR decoded with gamma 2.2, flags & 64: gamma 2.4, else the sRGB curve.
vec3 SdrDecode(vec3 c) { c = max(c, 0.0); return (pc.flags & 32u) != 0u ? pow(c, vec3(2.2)) : (pc.flags & 64u) != 0u ? pow(c, vec3(2.4)) : SrgbToLin(c); }
float clamp01(float x) { return clamp(x, 0.0, 1.0); }

// ---- RenoFX HDR Boost, ported from RenoFX by Carlos Lopez (ShortFuse), MIT licence:
// https://github.com/clshortfuse/renofx -- Copyright (c) 2026 Carlos Lopez. Permission is hereby granted, free of charge,
// to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the
// Software without restriction, including without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to
// do so, subject to the following conditions: The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
// C-infinity inverse tone mapping (HDR Boost, Start, Ramp Intensity) in the LMS working space with its hue restore, or
// by brightness (Yf), and the boost's colour blend. In: SDR linear BT.709 (1 = SDR white); out: the same, boosted.
float RfxTransition(float p)
{
    p = clamp01(p);
    if (p <= 0.0) return 0.0;
    if (p >= 1.0) return 1.0;
    return 1.0 / (1.0 + exp2((1.0 - 2.0 * p) / (p * (1.0 - p))));
}
float RfxRampAt(float x, float start, float ri)
{
    if (x <= start) return 0.0;
    float pos = (x - start) / max(1.0 - start, 1e-6);
    float w = pos / (1.0 + pos);
    return pos * pow(2.0 * RfxTransition(w), 1.0 / max(ri, 1e-3));
}
float RfxBoost1(float x, float mult, float peakRamp, float start, float ri)
{
    if (mult <= 1.0 || x <= start) return x;
    return x * (1.0 + (mult - 1.0) * RfxRampAt(x, start, ri) / max(peakRamp, 1e-6));
}
vec3 RfxToLms(vec3 c)
{
    return vec3(dot(vec3(0.2896057766, 0.6972466442, 0.0763712786), c),
                  dot(vec3(0.0901837576, 0.7073490257, 0.1122031464), c),
                  dot(vec3(0.0155287401, 0.0536093073, 0.5097978305), c));
}
vec3 RfxFromLms(vec3 c)
{
    return vec3(dot(vec3( 4.9676196501, -4.9223795290,  0.3391991813), c),
                  dot(vec3(-0.6196828788,  2.0517506515, -0.3587439845), c),
                  dot(vec3(-0.0861520039, -0.0658193831,  1.9889544835), c));
}
const vec3 kRfxLmsW = vec3(0.68990272, 0.34832189, 0.0371597069161);
float RfxYf(vec3 bt709) { return dot(RfxToLms(bt709), vec3(0.68990272, 0.34832189, 0.0)); }
vec3 RfxWeightedToMb(vec3 w)
{
    float y = max(w.x + w.y, 0.0);
    float iy = y > 0.0 ? 1.0 / y : 0.0;
    return vec3(w.x * iy, w.z * iy, y);
}
vec3 RfxMbToWeighted(vec2 c, float y) { return vec3(c.x, 1.0 - c.x, c.y) * y; }
vec2 RfxWhiteMb() { return RfxWeightedToMb(RfxToLms(vec3(1.0)) * kRfxLmsW).xy; }
float RfxHalfspaceT(vec2 o, vec2 d, vec2 n, float num)
{
    float den = dot(n, d);
    float an = num + dot(n, RfxWhiteMb() - o);
    return den > 1e-8 ? an / den : 1e20;
}
float RfxRayExitT(vec2 o, vec2 d)
{
    if (dot(d, d) <= 1e-14) return 1e20;
    float r = 1e20;
    r = min(r, RfxHalfspaceT(o, d, vec2(-0.043889, -0.006807), 0.0065035249));
    r = min(r, RfxHalfspaceT(o, d, vec2(-0.007821, -0.008564), 0.00104900495));
    r = min(r, RfxHalfspaceT(o, d, vec2(-0.000604, -0.007942), 0.000207697044));
    r = min(r, RfxHalfspaceT(o, d, vec2(0.0, -0.080835), 0.00165556648));
    r = min(r, RfxHalfspaceT(o, d, vec2(0.953597, 0.307020), 0.252472349));
    r = min(r, RfxHalfspaceT(o, d, vec2(-0.060969, 0.019752), 0.0241967351));
    r = min(r, RfxHalfspaceT(o, d, vec2(-0.106895, 0.004035), 0.0199621232));
    return max(r, 0.0);
}
vec3 RfxRestoreLmsHue(vec3 src, vec3 tgt)
{
    vec3 adapt = RfxToLms(vec3(1.0)) * 0.18;
    vec3 srcMb = RfxWeightedToMb(src * kRfxLmsW / adapt);
    vec3 tgtMb = RfxWeightedToMb(tgt * kRfxLmsW / adapt);
    vec2 neutral = RfxWeightedToMb(kRfxLmsW).xy;
    vec2 so = srcMb.xy - neutral, to = tgtMb.xy - neutral;
    float sr2 = dot(so, so), tr2 = dot(to, to);
    if (sr2 <= 1e-7 || tr2 <= 1e-7) return tgt;
    float tClipT = RfxRayExitT(neutral, to), tClipS = RfxRayExitT(neutral, so);
    float sp = tClipS > 0.0 ? clamp01(1.0 / tClipS) : 0.0;
    float tp = tClipT > 0.0 ? clamp01(1.0 / tClipT) : 0.0;
    float pw = sp > 1e-7 ? clamp01(tp / sp) : 1.0;
    float sens = mix(1.0, 0.35, clamp01((tClipT - 0.20139844) / (1.02634534 - 0.20139844)));
    float rw = sens * pw;
    if (rw <= 0.0) return tgt;
    float tr = sqrt(tr2);
    vec2 dir = mix(to / tr, so * inversesqrt(sr2), clamp01(rw));
    float dl = dot(dir, dir);
    dir = dl > 1e-7 ? dir * inversesqrt(dl) : to / tr;
    return RfxMbToWeighted(neutral + dir * tr, tgtMb.z) * max(adapt, vec3(1e-6)) / kRfxLmsW;
}
vec3 RfxHdrBoost(vec3 lin, float mult, float start, float ri, float colour, bool byYf)
{
    if (mult <= 1.0001) return lin;
    float peakRamp = RfxRampAt(1.0, start, ri);
    if (peakRamp <= 1e-6) return lin;
    vec3 boosted;
    if (byYf)
    {
        float wy = max(RfxYf(vec3(1.0)), 1e-6), sy = max(RfxYf(lin), 0.0);
        float by = RfxBoost1(sy / wy, mult, peakRamp, start, ri) * wy;
        boosted = sy > 1e-6 ? lin * (by / sy) : lin;
    }
    else
    {
        vec3 w = RfxToLms(lin), white = RfxToLms(vec3(1.0));
        vec3 n = max(vec3(0.0), w / white);
        vec3 b = vec3(RfxBoost1(n.x, mult, peakRamp, start, ri), RfxBoost1(n.y, mult, peakRamp, start, ri),
                          RfxBoost1(n.z, mult, peakRamp, start, ri)) * white;
        boosted = RfxFromLms(RfxRestoreLmsHue(w, b));
    }
    float sY = RfxYf(lin), bY = RfxYf(boosted);
    if (sY <= 1e-6 || bY <= 1e-6) return boosted;
    return mix(lin * (bY / sY), boosted, clamp01(colour));
}
vec3 SdrToHdr(vec3 lin)
{
    // flags & 256: RenoFX's HDR Boost (peak = SDR highlights / SDR white, Start in sdrKnee); else the knee curve.
    // RenoFX's availability blends each channel between the source and the full boost, which is the same as a smaller
    // boost: 1 + a (boost - 1). The APL Limiter's reduction (rfxAplCut = 1 - a) is applied that way.
    float rfxMult = pc.sdrPeak > pc.sdrScale ? pc.sdrPeak / pc.sdrScale : 1.0;
    rfxMult -= clamp(pc.rfxAplCut, 0.0, 1.0) * (rfxMult - 1.0);
    vec3 o = (pc.flags & 256u) != 0u ? RfxHdrBoost(max(lin, vec3(0.0)), rfxMult,
                                                   pc.sdrKnee, pc.rfxRamp, pc.rfxColor, (pc.flags & 512u) != 0u) * pc.sdrScale
                                       : SdrExpand(lin);
    if (pc.gamut > 0.0) o = ExpandGamut(o / pc.sdrScale, pc.gamut) * pc.sdrScale;
    return o;
}
// The SDR signal as linear light, before SDR to HDR (outType 4/5).
vec3 SdrIn(vec3 c) { return pc.outType == 5u ? ((pc.flags & 96u) != 0u ? SdrDecode(LinToSrgb(c)) : c) : SdrDecode(c); }
vec3 Decode(vec3 c)
{
    if (pc.outType == 0u) return SrgbToLin(c);
    if (pc.outType == 2u) return PqToLin(c);
    if (pc.outType == 4u) return SdrToHdr(SdrDecode(c));
    // 5: the hardware decoded the sRGB curve; for gamma 2.2 / 2.4 the signal is re-encoded first
    if (pc.outType == 5u) return SdrToHdr((pc.flags & 96u) != 0u ? SdrDecode(LinToSrgb(c)) : c);
    return c;
}
// BT.709 -> BT.2020 primaries (ITU-R BT.2087), for SDR games presented as HDR10. Every row is a positive blend
// summing to 1, so greys stay grey and no channel goes above the brightest input channel.
vec3 Bt709To2020(vec3 l)
{
    return vec3(dot(l, vec3(0.627404, 0.329283, 0.043313)),
                dot(l, vec3(0.069097, 0.919540, 0.011362)),
                dot(l, vec3(0.016391, 0.088013, 0.895595)));
}
vec3 Encode(vec3 l)
{
    if (pc.outType == 0u) return clamp(LinToSrgb(l), 0.0, 1.0);
    if (pc.outType >= 4u) return LinToPq(Bt709To2020(l));
    if (pc.outType == 2u) return LinToPq(l);
    if (pc.outType == 3u) return clamp(l, 0.0, 1.0);
    return l;
}
bool Inverted() { return (pc.flags & 4u) != 0u && pc.frames > 2u; }
float LitGain() { return Inverted() ? float(pc.frames) * pc.gain / float(pc.frames - 1u) : float(pc.frames) * pc.gain; }

float Target(uint j, float litGain, float fl)
{
    if ((pc.flags & 2u) != 0u)
    {
        float ph = (float(j) / float(pc.frames)) * 6.28318531;
        if (Inverted()) ph += 3.14159265;
        return mix(fl, litGain, 0.5 + 0.5 * cos(ph));
    }
    bool lit = Inverted() ? (j != 0u) : (j == 0u);
    return lit ? litGain : fl;
}
