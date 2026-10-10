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
// SDR shown as HDR10: SDR white at sdrScale, and above the knee the
// brightest channel is stretched toward sdrPeak (hue kept). Below the knee
// the image is untouched; the curve meets the straight part with the same
// slope, so there is no visible seam:  out = W*m + (P - W) * t^2.
vec3 SdrExpand(vec3 l)
{
    l = max(l, 0.0);
    float m = max(max(l.r, l.g), l.b);
    if (pc.sdrPeak <= pc.sdrScale || m <= pc.sdrKnee) return l * pc.sdrScale;
    float t = (min(m, 1.0) - pc.sdrKnee) / max(1.0 - pc.sdrKnee, 1e-4);
    float o = pc.sdrScale * m + (pc.sdrPeak - pc.sdrScale) * t * t;
    return l * (o / m);
}
vec3 Decode(vec3 c)
{
    if (pc.outType == 0u) return SrgbToLin(c);
    if (pc.outType == 2u) return PqToLin(c);
    if (pc.outType == 4u) return SdrExpand(SrgbToLin(c));
    if (pc.outType == 5u) return SdrExpand(c);
    return c;
}
vec3 Encode(vec3 l)
{
    if (pc.outType == 0u) return clamp(LinToSrgb(l), 0.0, 1.0);
    if (pc.outType == 2u || pc.outType >= 4u) return LinToPq(l);
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
