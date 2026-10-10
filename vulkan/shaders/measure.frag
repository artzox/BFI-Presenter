#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) out float outLum;

// ABL guard measurement: average luminance (80-nit units) of the lit refresh
// this frame would produce at the configured boost, over a 32x18 grid with
// 4x4 samples per cell.
void main()
{
    vec2 size = vec2(textureSize(src, 0));
    vec2 cell = size / vec2(32.0, 18.0);
    float litGain = LitGain();
    float sum = 0.0;
    for (int y = 0; y < 4; y++)
    for (int x = 0; x < 4; x++)
    {
        ivec2 p = ivec2((floor(gl_FragCoord.xy) + (vec2(x, y) + 0.5) / 4.0) * cell);
        vec3 lin = max(Decode(texelFetch(src, p, 0).rgb), 0.0);
        float m = max(max(lin.r, lin.g), lin.b);
        float S = (m > 1e-6) ? max(pc.ceilLin / m, 1.0) : 1e6;
        sum += dot(lin, vec3(0.2126, 0.7152, 0.0722)) * min(litGain, S);
    }
    outLum = sum / 16.0;
}
