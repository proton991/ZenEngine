#ifndef ZEN_PROBE_RAY
#define ZEN_PROBE_RAY
#include "probe_common.glsl"
#include "hybrid_provider.glsl"
#define HIT_LIGHT_CACHE
#include "hit_radiance.glsl"

// Keep the shared VoxelGIRenderer ray/hit binding contract even when this gather does not
// sample opacity or the previous radiance cache itself.
layout(set = 1, binding = 6) uniform sampler3D voxelOpacity;
layout(set = 1, binding = 7) uniform samplerCube coneEnvironmentMap;
layout(set = 1, binding = 13) uniform sampler3D hitRadiance;
layout(set = 1, binding = 14) uniform sampler3D hitNormal;
layout(set = 1, binding = 15) uniform sampler3D hitRadiancePrevious;

// VoxelGISettings::Feature, shared with the per-pixel gather's provider mask.
const uint SKY_PROVIDER        = 1u;
const uint SPECULAR_PROVIDER   = 2u;
const uint BOUNCE_PROVIDER     = 8u;
const uint REFLECTION_PROVIDER = 16u;

bool ProbeFeatureHardware(uint feature)
{
    return (hybrid.provider.z & feature) != 0u;
}

HybridRayResult ProbeTraceFeature(uint feature, vec3 position, vec3 normal, vec3 direction, bool closest)
{
    // A HW-only variant has no Compute provider: a feature it does not trace has no result.
    HybridRayResult result = HybridRayResult(false, position, vec3(0), ivec3(-1), false);
#ifdef HYBRID_HARDWARE_PROVIDER
    if (ProbeFeatureHardware(feature))
    {
        result = HardwareTraceRay(HybridTraceOrigin(position, normal, 0.0), direction, 1e20, closest);
    }
#endif
#ifdef HYBRID_FIELD_TRAVERSAL
    if (!ProbeFeatureHardware(feature))
    {
        result = FieldTraceRay(FieldTraceOrigin(position, normal), direction, 1e20, closest);
    }
#endif
    return result;
}

// Visibility of a nearby point or spot light from a shading hit (hit_radiance.glsl), with the bounce
// provider, over the finite segment to the light. Only a covered miss is visible.
float HitLightVisibility(HybridRayResult hit, SceneLight light, int lightIndex)
{
    float visibility = 1.0;
#ifdef HYBRID_HARDWARE_PROVIDER
    if (ProbeFeatureHardware(BOUNCE_PROVIDER))
    {
        vec3  origin          = HybridTraceOrigin(hit.position, hit.normal, 0.0);
        vec3  delta           = light.positionRange.xyz - origin;
        float distanceToLight = length(delta);
        vec3  direction       = delta / max(distanceToLight, 1e-20);
        // Exclude the endpoint itself, without dropping a finite interval before it.
        distanceToLight = uintBitsToFloat(floatBitsToUint(distanceToLight) - uint(distanceToLight > 0.0));
        visibility      = distanceToLight > 0.0 && HardwareTraceRay(origin, direction, distanceToLight, false).hit ? 0.0 : 1.0;
    }
    else
#endif
    {
#ifdef HYBRID_FIELD_TRAVERSAL
        visibility = FieldLightVisibility(hit, light.positionRange.xyz);
#endif
    }
    return visibility;
}
#endif
