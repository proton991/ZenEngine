#version 450
#extension GL_GOOGLE_include_directive : require
#include "debug_view.glsl"
#include "../Common/gbuffer.glsl"
layout(set=1,binding=0) uniform sampler2D sourceImage;
layout(set=1,binding=2) uniform sampler2D depthMap;
void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec3 color = vec3(0.0);
    if (texelFetch(depthMap, pixel, 0).r < 1.0)
        color = DecodeGBufferNormal(texelFetch(sourceImage, pixel, 0).rg) * 0.5 + 0.5;
    outColor = vec4(color, 1.0);
}
