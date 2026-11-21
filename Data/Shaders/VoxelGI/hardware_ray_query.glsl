#ifndef ZEN_HARDWARE_RAY_QUERY
#define ZEN_HARDWARE_RAY_QUERY
#include "hybrid_ray_counts.glsl"
#include "scene_ray_inputs.glsl"
#include "hardware_visibility.glsl"

// The same query supports visibility and closest-hit consumers. Metadata is
// versioned with the snapshot, not decoded against a later scene generation.
struct HardwareRayHit
{
    HybridRayResult ray;
    uint            instance;
    uint            primitive;
    uint            material;
    vec2            barycentrics;
    uvec2           generation;
    // Facing before orienting the geometric normal against the incoming ray. Every face occludes.
    bool frontFacing;
};
// Share clipping, alpha testing and ray accounting between visibility and surface queries.
bool HardwareTraverse(rayQueryEXT query, vec3 origin, vec3 direction, float maximumDistance, bool closest)
{
    float end;
    bool  intersects = HardwareRayInterval(origin, direction, maximumDistance, end);
    if (intersects)
    {
#ifdef HYBRID_COUNT_RAYS
        ++issuedHardwareRays[issuedRayFeature];
        ++traceBreakdown[issuedRayPurpose != 0u ? issuedRayPurpose : (issuedRayFeature == 3u ? 3u : 0u)];
#endif
        uint flags = closest ? 0u : gl_RayFlagsTerminateOnFirstHitEXT;
        // Pad the scene bound, but never trace beyond a finite caller-supplied endpoint (a light).
        rayQueryInitializeEXT(query, sceneAccelerationStructure, flags, 0xffu, origin, 0.0, direction, end);
        while (rayQueryProceedEXT(query))
        {
#ifdef HYBRID_COUNT_RAYS
            ++hardwareCandidates[issuedRayFeature];
#endif
            if (rayQueryGetIntersectionTypeEXT(query, false) == gl_RayQueryCandidateIntersectionTriangleEXT)
            {
                uvec4 triangle = RayTriangle(rayQueryGetIntersectionInstanceCustomIndexEXT(query, false),
                                             rayQueryGetIntersectionGeometryIndexEXT(query, false),
                                             rayQueryGetIntersectionPrimitiveIndexEXT(query, false));
                if (RayCandidateAccepted(triangle, rayQueryGetIntersectionBarycentricsEXT(query, false)))
                {
                    rayQueryConfirmIntersectionEXT(query);
                }
            }
        }
    }
    return intersects;
}
bool HardwareOccluded(vec3 origin, vec3 direction, float maximumDistance)
{
    rayQueryEXT query;
    bool        occluded = false;
    if (HardwareTraverse(query, origin, direction, maximumDistance, false))
    {
        occluded = rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionTriangleEXT;
    }
    return occluded;
}

HardwareRayHit HardwareQuery(vec3 origin, vec3 direction, float maximumDistance, bool closest)
{
    HardwareRayHit result;
    result.ray      = MakeHybridRayResult(false, origin, vec3(0), ivec3(-1), true);
    result.instance = result.primitive = result.material = 0xffffffffu;
    result.barycentrics                                  = vec2(0);
    result.generation                                    = rayScene.generation.xy;
    result.frontFacing                                   = false;
    rayQueryEXT query;
    if (HardwareTraverse(query, origin, direction, maximumDistance, closest))
    {
        if (rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionTriangleEXT)
        {
            result.instance  = rayQueryGetIntersectionInstanceCustomIndexEXT(query, true);
            result.primitive = rayQueryGetIntersectionPrimitiveIndexEXT(query, true);
            uvec4 triangle =
                RayTriangle(result.instance, rayQueryGetIntersectionGeometryIndexEXT(query, true), result.primitive);
            result.material         = triangle.z;
            result.barycentrics     = rayQueryGetIntersectionBarycentricsEXT(query, true);
            result.ray.triangle     = triangle;
            result.ray.barycentrics = result.barycentrics;
            result.ray.hit          = true;
            uvec3 vertexIndices     = RayVertexIndices(triangle);
            RaySurfaceGeometry(triangle, vertexIndices, result.barycentrics, result.ray.position, result.ray.normal);
            result.frontFacing = dot(result.ray.normal, direction) < 0.0;
            if (!result.frontFacing)
            {
                result.ray.normal = -result.ray.normal;
            }
            result.ray.cell = ivec3(floor((result.ray.position - gi.gridMinVoxelSize.xyz) / gi.gridMinVoxelSize.w));
        }
    }
    return result;
}
HybridRayResult HardwareTraceRay(vec3 origin, vec3 direction, float maximumDistance, bool closest)
{
    return HardwareQuery(origin, direction, maximumDistance, closest).ray;
}
#endif
