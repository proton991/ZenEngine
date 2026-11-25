#version 460
#extension GL_GOOGLE_include_directive : require
#include "debug_view.glsl"
layout(set = 1, binding = 0) uniform sampler2D worldIrradiance;
void main()
{
    vec4 value = textureLod(worldIrradiance, inUV, 0);
    // Mode 1 displays history age from 0 to 256. Mode 2 shows the per-pixel coverage mask.
    if (debug.selection.x == 2u)
    {
        ivec2 extent = textureSize(worldIrradiance, 0);
        ivec2 pixel  = clamp(ivec2(inUV * vec2(extent)), ivec2(0), extent - 1);
        outColor     = vec4(vec3(float(texelFetch(worldIrradiance, pixel, 0).a < 0.0)), 1);
    }
    else
    {
        outColor = debug.selection.x == 1u ? vec4(vec3(clamp(value.a / 256.0, 0.0, 1.0)), 1)
                                           : vec4(DisplaySRGB(value.rgb / (vec3(1) + value.rgb)), 1);
    }
}
