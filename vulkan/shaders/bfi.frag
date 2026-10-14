#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) out vec4 outColor;

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
vec3 Strobe(vec3 lin, uint j, float litGain)
{
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

void main()
{
    vec3 c = texelFetch(src, ivec2(gl_FragCoord.xy), 0).rgb;
    if (pc.method == 2u) { outColor = vec4(pc.outType >= 4u ? Encode(Decode(c)) : c, 1.0); return; }
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
        float phi = (gl_FragCoord.y / float(textureSize(src, 0).y)) * n;
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
        float u = (gl_FragCoord.y / H) * float(pc.frames);
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
    if (pc.soften > 0.0 && pc.method < 2u)
    {
        // LCD phase swap: fade toward the steady average picture (the hitch guard's), which keeps the light
        // of every cycle the same while the strobe's depth goes down and back up.
        float a = 0.0;
        for (uint j = 0u; j < 8u; j++) if (j < pc.frames) a += Target(j, litGain, pc.dfloor);
        o = mix(o, lin * (a / float(pc.frames)), pc.soften);
    }
    outColor = vec4(Encode(o), 1.0);
}
