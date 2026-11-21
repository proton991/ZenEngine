#ifndef ZEN_HARDWARE_RAY_PIPELINE
#define ZEN_HARDWARE_RAY_PIPELINE
#include "ray_radiance_payload.glsl"
#define RAY_SHADOW_MISS 1u
// Primary and shadow rays have different payload types and use separate hit groups.
#define RAY_SHADOW_HIT 1u
#include "ray_shadow.glsl"
#ifdef HYBRID_CLOSEST_HIT
layout(location = 0) rayPayloadInEXT HybridRadiancePayload hitPayload;
#else
layout(location = 0) rayPayloadEXT HybridRadiancePayload hitPayload;
uvec3 rayShading = uvec3(0);
#    ifdef HYBRID_RADIANCE_CACHE
float rayCacheCone = -1.0;
#    endif
HybridRayResult HardwareTraceRay(vec3 origin, vec3 direction, float maximumDistance, bool closest)
{
    HybridRayResult result = MakeHybridRayResult(false, origin, vec3(0), ivec3(-1), true);
    if (closest)
    {
        float end;
        if (HardwareRayInterval(origin, direction, maximumDistance, end))
        {
            hitPayload.material      = 0xffffffffu;
            hitPayload.radiance      = vec3(0);
            hitPayload.seed          = rayShading.x;
            hitPayload.skipNormalMap = rayShading.y;
#    ifdef HYBRID_RADIANCE_CACHE
            hitPayload.cacheCone = rayCacheCone;
#    endif
#    ifdef HYBRID_COUNT_RAYS
            ++issuedHardwareRays[issuedRayFeature];
            ++traceBreakdown[issuedRayPurpose != 0u ? issuedRayPurpose : (issuedRayFeature == 3u ? 3u : 0u)];
            hitPayload.rays            = uvec4(0);
            hitPayload.breakdown       = uvec4(0);
            hitPayload.candidates      = uvec4(0);
            hitPayload.primaryFeature  = issuedRayFeature;
            hitPayload.lightingFeature = rayShading.z;
#        ifdef HYBRID_RADIANCE_CACHE
            hitPayload.cacheCounts = uvec2(0);
#        endif
#    endif
            ZenTraceRay(sceneAccelerationStructure, 0u, 0xffu, 0u, 0u, 0u, origin, 0.0, direction, end, 0);
#    ifdef HYBRID_COUNT_RAYS
            issuedHardwareRays += hitPayload.rays;
            traceBreakdown     += hitPayload.breakdown;
            hardwareCandidates += hitPayload.candidates;
#        ifdef HYBRID_RADIANCE_CACHE
            cacheTraceCounts += hitPayload.cacheCounts;
#        endif
#    endif
            if (hitPayload.material != 0xffffffffu)
            {
                result.hit      = true;
                result.position = hitPayload.position;
                result.normal   = hitPayload.normal;
                result.cell     = ivec3(floor((result.position - gi.gridMinVoxelSize.xyz) / gi.gridMinVoxelSize.w));
            }
        }
    }
    else
    {
        result.hit = HardwareOccluded(origin, direction, maximumDistance);
    }
    return result;
}
#endif
#endif
