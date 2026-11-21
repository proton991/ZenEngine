#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_tracing : require
#include "gi_common.glsl"
#include "scene_ray_inputs.glsl"
layout(location = 0) rayPayloadInEXT uvec4 occluderTriangle;
void main()
{
    occluderTriangle = RayTriangle(gl_InstanceCustomIndexEXT, gl_GeometryIndexEXT, gl_PrimitiveID);
}
