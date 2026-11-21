#include "hybrid_sampling.glsl"
#include "hybrid_provider.glsl"
layout(set = 1, binding = 7) uniform samplerCube coneEnvironmentMap;
#include "ray_hit_lighting.glsl"
hitAttributeEXT vec2 barycentrics;
#ifdef HYBRID_HIT_RECORD
// Report only which triangle was hit and where. A later pass reconstructs and shades the hit,
// so this pipeline's registers and ray stack stay those of traversal.
void main()
{
    hitPayload.material = 0u;
    hitPayload.position = uintBitsToFloat(uvec3(gl_InstanceCustomIndexEXT, gl_GeometryIndexEXT, gl_PrimitiveID));
    hitPayload.normal   = vec3(barycentrics, 0.0);
}
#else
void main()
{
    uvec4           triangle = RayTriangle(gl_InstanceCustomIndexEXT, gl_GeometryIndexEXT, gl_PrimitiveID);
    HybridRayResult hit      = MakeHybridRayResult(true, vec3(0), vec3(0), ivec3(-1), true);
    hit.triangle             = triangle;
    hit.barycentrics         = barycentrics;
    RaySurfaceGeometry(triangle, RayVertexIndices(triangle), barycentrics, hit.position, hit.normal);
    if (dot(hit.normal, gl_WorldRayDirectionEXT) >= 0.0)
    {
        hit.normal = -hit.normal;
    }
    hit.cell            = ivec3(floor((hit.position - gi.gridMinVoxelSize.xyz) / gi.gridMinVoxelSize.w));
    hitSkipsNormalMap   = (hitPayload.skipNormalMap & 1u) != 0u;
    hitPayload.position = hit.position;
    hitPayload.normal   = hit.normal;
    hitPayload.material = hit.triangle.z;
#    ifdef HYBRID_COUNT_RAYS
    issuedRayFeature = hitPayload.lightingFeature;
#    endif
#    ifdef HYBRID_RADIANCE_CACHE
    hitCacheSegment = gl_HitTEXT;
    hitCacheCone    = hitPayload.cacheCone;
#    endif
    hitPayload.radiance = HardwareHitRadiance(hit, hitPayload.seed);
#    ifdef HYBRID_COUNT_RAYS
    hitPayload.rays        = issuedHardwareRays;
    hitPayload.breakdown   = traceBreakdown;
    hitPayload.candidates += hardwareCandidates;
#        ifdef HYBRID_RADIANCE_CACHE
    hitPayload.cacheCounts = cacheTraceCounts;
#        endif
#    endif
}
#endif
