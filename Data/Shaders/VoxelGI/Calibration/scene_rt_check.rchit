#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_tracing : require
#include "../gi_common.glsl"
#include "../scene_ray_inputs.glsl"
hitAttributeEXT vec2 barycentrics;
layout(location = 0) rayPayloadInEXT vec4 result;
void main()
{
    uvec4 triangle = RayTriangle(gl_InstanceCustomIndexEXT, gl_GeometryIndexEXT, gl_PrimitiveID);
    vec3  position = RaySurfacePosition(triangle, barycentrics);
    result         = vec4(position.z - gl_WorldRayOriginEXT.z, float(triangle.z), float(gl_InstanceCustomIndexEXT),
                          float(rayScene.generation.x));
}
