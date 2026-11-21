#ifdef HYBRID_RAY_PIPELINE
#    include "../VoxelGI/gi_common.glsl"
#    include "../VoxelGI/hybrid_ray_counts.glsl"
#    include "../VoxelGI/scene_ray_inputs.glsl"
#    include "../VoxelGI/hardware_ray_interval.glsl"
layout(location = 0) rayPayloadEXT uvec4 occluderTriangle;
#else
#    include "../Common/opacity_micromap.glsl"
#    define HYBRID_HARDWARE_PROVIDER
#    include "../VoxelGI/hybrid_provider.glsl"
#endif
#define BUILD_LIGHT_OCCLUDERS
#include "light_occluders.glsl"
#include "../VoxelGI/gi_ray_totals.glsl"
#ifndef HYBRID_RAY_PIPELINE
layout(local_size_x = 8, local_size_y = 8) in;
#endif
layout(std430, set = 6, binding = 6) writeonly buffer LightOccluders
{
    LightOccluder lightOccluders[];
};
uvec4 TraceLightOccluder(vec3 origin, vec3 direction)
{
    uvec4 triangle = uvec4(0xffffffffu);
#ifdef HYBRID_RAY_PIPELINE
    float end;
    if (HardwareRayInterval(origin, direction, 1e20, end))
    {
#    ifdef HYBRID_COUNT_RAYS
        ++issuedHardwareRays[issuedRayFeature];
#    endif
        occluderTriangle = triangle;
        ZenTraceRay(sceneAccelerationStructure, 0u, 0xffu, 0u, 0u, 0u, origin, 0.0, direction, end, 0);
        triangle = occluderTriangle;
    }
#else
    HardwareRayHit hit = HardwareQuery(origin, direction, 1e20, true);
    if (hit.ray.hit)
    {
        triangle = hit.ray.triangle;
    }
#endif
    return triangle;
}
void main()
{
#ifdef HYBRID_RAY_PIPELINE
    uvec3 invocation = gl_LaunchIDEXT;
#else
    uvec3 invocation = gl_GlobalInvocationID;
#endif
    ivec2         pixel      = ivec2(invocation.xy);
    int           face       = int(invocation.z) % 6;
    int           lightIndex = int(invocation.z) / 6;
    LightOccluder result     = LightOccluder(vec4(0), vec4(0), vec4(0));
    SceneLight    light      = sceneUbo.lights[lightIndex];
    if (lightIndex < int(sceneUbo.lightInfo.x) && int(light.directionType.w) != 0 && light.coneShadow.z > 0.0)
    {
        int   axis        = face / 2;
        ivec2 axes        = LightOccluderAxes(axis);
        vec2  uv          = (vec2(pixel) + 0.5) / float(LIGHT_OCCLUDER_SIZE) * 2.0 - 1.0;
        vec3  direction   = vec3(0);
        direction[axis]   = face % 2 == 0 ? 1.0 : -1.0;
        direction[axes.x] = uv.x;
        direction[axes.y] = uv.y;
        uvec4 triangle    = TraceLightOccluder(light.positionRange.xyz, normalize(direction));
        if (triangle.x != 0xffffffffu && materialData[triangle.z].surfaceProperties.y == 0.0)
        {
            vec3 a        = RaySurfacePosition(triangle, vec2(0));
            result.origin = vec4(a, 1);
            result.edge1  = vec4(RaySurfacePosition(triangle, vec2(1, 0)) - a, 0);
            result.edge2  = vec4(RaySurfacePosition(triangle, vec2(0, 1)) - a, 0);
            // Reused by every receiver testing this candidate triangle.
            result.edge1.w = length(cross(result.edge1.xyz, result.edge2.xyz));
        }
    }
    lightOccluders[LightOccluderIndex(lightIndex, face, pixel)] = result;
#ifdef GI_COUNT_RAY_SLOT
    RecordGIRays();
#endif
}
