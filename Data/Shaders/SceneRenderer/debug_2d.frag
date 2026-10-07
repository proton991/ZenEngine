#version 450
#extension GL_GOOGLE_include_directive : require
#include "debug_view.glsl"
layout(set=1,binding=0) uniform sampler2D sourceImage;
void main()
{
    vec4 value = texture(sourceImage, inUV);
    vec3 color = value.rgb;
    if (debug.selection.x == 1u) color = vec3(DisplayDepth(value.r));
    if (debug.selection.x == 2u) color = DisplaySRGB(value.rgb);
    outColor = vec4(color, 1);
}
