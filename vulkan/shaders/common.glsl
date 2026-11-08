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
vec3 SdrToHdr(vec3 lin)
{
    vec3 o = SdrExpand(lin);
    if (pc.gamut > 0.0) o = ExpandGamut(o / pc.sdrScale, pc.gamut) * pc.sdrScale;
    return o;
}
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
