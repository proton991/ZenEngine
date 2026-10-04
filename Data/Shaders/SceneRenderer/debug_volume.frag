#version 450
#extension GL_GOOGLE_include_directive : require
#include "debug_view.glsl"
layout(set=1,binding=0) uniform sampler3D sourceImage;
void main()
{
    ivec3 size = textureSize(sourceImage, 0);
    vec3 uv = vec3(inUV, (float(debug.selection.w) + 0.5) / float(size[debug.selection.z]));
    if (debug.selection.z == 0u) uv = uv.zxy;
    if (debug.selection.z == 1u) uv = uv.xzy;
    vec4 value = textureLod(sourceImage, uv, 0.0);
    outColor = vec4(value.a > 0.0 ? DisplaySRGB(value.rgb) : vec3(0), 1);
}
