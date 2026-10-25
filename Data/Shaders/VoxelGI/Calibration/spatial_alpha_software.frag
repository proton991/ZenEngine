#version 450
#extension GL_GOOGLE_include_directive : require
#define MATERIAL_FRAGMENT    1
#define RAY_FORWARD_MATERIAL 1
#define MATERIAL_BINDING_UV  1
// Raster UVs differ from each ray hit's UVs; ray acceptance must use the latter.
#define MATERIAL_BINDING_UV_VALUE(slot) (gl_FragCoord.xy / vec2(128.0, 192.0))
#include "spatial_alpha_query.glsl"
layout(location = 0) out vec4 color;
void main()
{
    TraceSpatialAlpha(uint(gl_FragCoord.y) * 128u + uint(gl_FragCoord.x));
    color = vec4(1);
}
