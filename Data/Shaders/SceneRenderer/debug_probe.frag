#version 450
#extension GL_GOOGLE_include_directive : require
#include "debug_view.glsl"
layout(set = 1, binding = 0) uniform sampler2D sourceImage;
layout(set = 1, binding = 2) uniform sampler2D pixelHistory;
void main()
{
    ivec2 pixel  = clamp(ivec2(inUV * vec2(textureSize(sourceImage, 0))), ivec2(0), textureSize(sourceImage, 0) - 1);
    vec4  value  = texelFetch(sourceImage, pixel, 0);
    float frames = texelFetch(pixelHistory, pixel, 0).w;
    vec3  color  = vec3(0);
    if (frames > 0.0)
    {
        if (debug.selection.x <= 1u)
        {
            color = DisplaySRGB(value.rgb);
        }
        else if (debug.selection.x == 2u)
        {
            color = vec3(frames / max(debug.range.y, 1.0));
        }
        else
        {
            // Blue marks receivers without a resolved specular sample this frame.
            color = value.a < 0.0 ? vec3(0, 0, 1) : vec3(value.a);
        }
    }
    outColor = vec4(color, 1);
}
