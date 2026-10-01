#version 450
#extension GL_GOOGLE_include_directive : require
#include "../Common/bindless_heap.glsl"
#include "../Common/linear_to_srgb.glsl"
layout(location=0) in vec2 inUV;
layout(location=0) out vec4 outColor;
layout(set=1,binding=0) uniform sampler2D sceneColorMap;
layout(push_constant) uniform ToneMapConstants { uint mode; } toneMap;
void main()
{
    vec3 color = texture(sceneColorMap, inUV).rgb;
    if (toneMap.mode != 2u) color = color / (color + vec3(1.0));
    if (toneMap.mode != 1u) color = LinearToSRGB(color);
    outColor = vec4(color, 1.0);
}
