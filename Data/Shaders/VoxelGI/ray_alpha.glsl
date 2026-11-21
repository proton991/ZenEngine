#include "gi_common.glsl"
#include "scene_ray_inputs.glsl"
#if defined(RAY_ALPHA_RADIANCE) || defined(RAY_ALPHA_RADIANCE_COUNTS)
#    include "ray_radiance_payload.glsl"
layout(location = 0) rayPayloadInEXT HybridRadiancePayload hitPayload;
#elif defined(RAY_ALPHA_TRIANGLE)
layout(location = 0) rayPayloadInEXT uvec4 occluderTriangle;
#else
layout(location = 1) rayPayloadInEXT uint shadowed;
#endif
hitAttributeEXT vec2 barycentrics;
void                 main()
{
#ifdef RAY_ALPHA_RADIANCE_COUNTS
    ++hitPayload.candidates[hitPayload.primaryFeature];
#elif defined(RAY_ALPHA_SHADOW_COUNTS)
    shadowed += 2u;
#endif
    uvec4 triangle = RayTriangle(gl_InstanceCustomIndexEXT, gl_GeometryIndexEXT, gl_PrimitiveID);
    if (!RayCandidateAccepted(triangle, barycentrics))
    {
        ignoreIntersectionEXT;
    }
}
