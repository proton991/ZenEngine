#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_query : require
#define HYBRID_HARDWARE_PROVIDER        1
#define MATERIAL_FRAGMENT               1
#define RAY_FORWARD_MATERIAL            1
#define MATERIAL_BINDING_UV             1
#define MATERIAL_BINDING_UV_VALUE(slot) (gl_FragCoord.xy / vec2(128.0, 192.0))
#include "spatial_alpha_query.glsl"
layout(location = 0) out vec4 color;
void main()
{
    TraceSpatialAlpha(uint(gl_FragCoord.y) * 128u + uint(gl_FragCoord.x));
    color = vec4(1);
}
