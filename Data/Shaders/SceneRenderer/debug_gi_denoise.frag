#version 450
#extension GL_GOOGLE_include_directive : require
#include "debug_view.glsl"
layout(set = 1, binding = 0) uniform sampler2DArray validationImage;
void main()
{
    ivec2 extent = textureSize(validationImage, 0).xy;
    ivec2 pixel  = clamp(ivec2(inUV * vec2(extent)), ivec2(0), extent - 1);
    uint  mode   = debug.selection.x;
    uint  layer  = mode == 5u ? 2u : mode == 6u ? 0u : mode < 3u ? mode : mode - 3u;
    vec4  value  = texelFetch(validationImage, ivec3(pixel, int(layer)), 0);
    vec3  color  = value.rgb / max(debug.range.y, 1.0);
    if (mode == 2u)
    {
        color = floor(value.rgb / 16.0) * 0.5;
    }
    else if (mode == 5u)
    {
        color = vec3(uvec3(value.rgb) & 15u) / 8.0;
    }
    else if (mode == 6u)
    {
        color = vec3(value.a / max(debug.range.y, 1.0), texelFetch(validationImage, ivec3(pixel, 1), 0).a / 4.0,
                     float(uint(texelFetch(validationImage, ivec3(pixel, 2), 0).a) & 15u) / 8.0);
    }
    outColor = vec4(clamp(color, vec3(0), vec3(1)), 1);
}
