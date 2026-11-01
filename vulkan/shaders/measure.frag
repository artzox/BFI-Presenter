#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) out float outLum;
#include "measure_cell.glsl"

void main() { outLum = MeasureCell(gl_FragCoord.xy); }
