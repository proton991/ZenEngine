#ifndef ZEN_RAY_RADIANCE_PAYLOAD
#define ZEN_RAY_RADIANCE_PAYLOAD
#include "hybrid_ray_result.glsl"
struct HybridRadiancePayload
{
    // Raygen consumes shaded radiance and the geometry used for reservoir reuse.
    // Triangle indices and barycentrics stay local to closest-hit material evaluation.
    vec3 position;
    vec3 normal;
    vec3 radiance;
    uint material;
    uint seed;
    uint skipNormalMap;
#ifdef HYBRID_RADIANCE_CACHE
    float cacheCone;
#endif
#ifdef HYBRID_COUNT_RAYS
    uvec4 breakdown;
    uvec4 rays;
    uvec4 candidates;
    uint  primaryFeature;
    uint  lightingFeature;
#    ifdef HYBRID_RADIANCE_CACHE
    uvec2 cacheCounts;
#    endif
#endif
};
#endif
