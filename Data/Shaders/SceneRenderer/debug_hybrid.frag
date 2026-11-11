#version 450
#extension GL_GOOGLE_include_directive : require
#include "debug_view.glsl"
layout(set = 1, binding = 0) uniform sampler2D sourceImage;
layout(set = 1, binding = 2) uniform sampler2D historyLength;
layout(set = 1, binding = 3) uniform usampler2D missingReceiver;
void main()
{
    ivec2 pixel   = clamp(ivec2(inUV * vec2(textureSize(sourceImage, 0))), ivec2(0), textureSize(sourceImage, 0) - 1);
    vec4  value   = texelFetch(sourceImage, pixel, 0);
    vec4  history = texelFetch(historyLength, pixel, 0);
    vec3  color   = vec3(0);
    // selection.y selects the reflection history (face 1); selection.z marks a real fallback mask.
    bool reflection = debug.selection.y != 0u;
    if (debug.selection.x == 5u)
    {
        ivec2      extent    = textureSize(missingReceiver, 0);
        ivec2      full      = clamp(ivec2(inUV * vec2(extent)), ivec2(0), extent - 1);
        uint       missing   = debug.selection.z != 0u ? texelFetch(missingReceiver, full, 0).r & 3u : 0u;
        const vec3 traced[4] = vec3[4](vec3(0.1), vec3(1, 0, 0), vec3(0, 0, 1), vec3(1, 0, 1));
        color                = history.r > 0.0 || missing != 0u ? traced[missing] : vec3(0);
    }
    else if (history.r > 0.0)
    {
        if (debug.selection.x <= 1u)
        {
            color = DisplaySRGB(value.rgb);
        }
        else if (debug.selection.x == 2u)
        {
            color = vec3((reflection ? value.a : history.r) / max(debug.range.y, 1.0));
        }
        else if (debug.selection.x == 3u)
        {
            color = vec3(value.a);
        }
        else
        {
            // hybrid_temporal.glsl packs the diffuse reason plus 16 times the reflection reason.
            const vec3 reasons[9] = vec3[9](vec3(0, 1, 0), vec3(1, 0, 0), vec3(1, 1, 0), vec3(1, 0, 1), vec3(0, 1, 1),
                                            vec3(0, 0, 1), vec3(1, 0.5, 0), vec3(1), vec3(0.5));
            float      code       = reflection ? floor(history.b / 16.0) : mod(history.b, 16.0);
            color                 = reasons[clamp(int(code), 0, 8)];
        }
    }
    outColor = vec4(color, 1);
}
