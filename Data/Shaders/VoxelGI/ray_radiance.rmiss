#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_tracing : require
#include "ray_radiance_payload.glsl"
layout(location = 0) rayPayloadInEXT HybridRadiancePayload hitPayload;
void main()
{
    hitPayload.material = 0xffffffffu;
    hitPayload.radiance = vec3(0);
}
