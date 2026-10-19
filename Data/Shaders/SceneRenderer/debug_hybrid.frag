#version 450
#extension GL_GOOGLE_include_directive : require
#include "debug_view.glsl"
layout(set = 1, binding = 0) uniform sampler2D sourceImage;
layout(set = 1, binding = 2) uniform sampler2D historyLength;
void main()
{
    ivec2 pixel   = clamp(ivec2(inUV * vec2(textureSize(sourceImage, 0))), ivec2(0), textureSize(sourceImage, 0) - 1);
    vec4  value   = texelFetch(sourceImage, pixel, 0);
    vec4  history = texelFetch(historyLength, pixel, 0);
    vec3  color   = vec3(0);
    if (history.r > 0.0)
    {
        if (debug.selection.x <= 1u)
        {
            color = DisplaySRGB(value.rgb);
        }
        else if (debug.selection.x == 2u)
        {
            color = vec3(history.r / max(debug.range.y, 1.0));
        }
        else if (debug.selection.x == 3u)
        {
            color = vec3(value.a);
        }
        else
        {
            const vec3 reasons[5] = vec3[5](vec3(0, 1, 0), vec3(1, 0, 0), vec3(1, 1, 0), vec3(1, 0, 1), vec3(0, 1, 1));
            color                 = reasons[clamp(int(history.b), 0, 4)];
        }
    }
    outColor = vec4(color, 1);
}
