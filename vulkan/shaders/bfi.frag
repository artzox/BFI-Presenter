#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) out vec4 outColor;
#include "bfi_shade.glsl"

void main() { outColor = BfiShade(gl_FragCoord.xy); }
