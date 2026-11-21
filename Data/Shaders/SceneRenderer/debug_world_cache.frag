#version 460
#extension GL_GOOGLE_include_directive : require
#include "debug_view.glsl"
layout(set = 1, binding = 0) uniform sampler2D worldIrradiance;
void main()
{
    vec4 value = textureLod(worldIrradiance, inUV, 0);
    // Mode 1: the probe history age in alpha, linear from 0 to 256 frames.
    outColor = debug.selection.x == 1u ? vec4(vec3(clamp(value.a / 256.0, 0.0, 1.0)), 1)
                                       : vec4(DisplaySRGB(value.rgb / (vec3(1) + value.rgb)), 1);
}
