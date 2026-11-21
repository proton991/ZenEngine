#ifndef ZEN_RAY_SHADOW
#define ZEN_RAY_SHADOW
#include "hybrid_ray_counts.glsl"
#include "scene_ray_inputs.glsl"
#include "hardware_visibility.glsl"
// Plain tracing clears the low payload bit on a miss. Hit-object traversal reads
// the hit directly; both paths retain any-hit candidate counts in the upper bits.
layout(location = 1) rayPayloadEXT uint shadowed;
#ifndef RAY_SHADOW_MISS
#    define RAY_SHADOW_MISS 0u
#endif
#ifndef RAY_SHADOW_HIT
#    define RAY_SHADOW_HIT 0u
#endif
bool HardwareOccluded(vec3 origin, vec3 direction, float maximumDistance)
{
    float end;
    bool  occluded = false;
    if (HardwareRayInterval(origin, direction, maximumDistance, end))
    {
#ifdef HYBRID_COUNT_RAYS
        ++issuedHardwareRays[issuedRayFeature];
        ++traceBreakdown[issuedRayPurpose != 0u ? issuedRayPurpose : (issuedRayFeature == 3u ? 3u : 0u)];
#endif
        shadowed = 1u;
#if defined(ZEN_INVOCATION_REORDER_EXT) || defined(ZEN_INVOCATION_REORDER_NV)
        // Visibility needs no closest-hit or miss shader execution.
        ZenHitObject hit;
        ZenTraceHit(hit, sceneAccelerationStructure, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT,
                    0xffu, RAY_SHADOW_HIT, 0u, RAY_SHADOW_MISS, origin, 0.0, direction, end, 1);
#    ifdef ZEN_REORDER_VISIBILITY
        // ReSTIR's reservoir processing benefits from coherent visibility results.
        ZenReorderHit(hit);
#    endif
        occluded = ZenHitIsHit(hit);
#else
        ZenTraceRay(sceneAccelerationStructure, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT, 0xffu,
                    RAY_SHADOW_HIT, 0u, RAY_SHADOW_MISS, origin, 0.0, direction, end, 1);
        occluded = (shadowed & 1u) != 0u;
#endif
#ifdef HYBRID_COUNT_RAYS
        hardwareCandidates[issuedRayFeature] += shadowed >> 1u;
#endif
    }
    return occluded;
}
#endif
