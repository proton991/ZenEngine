#ifndef ZEN_PROBE_RAY
#define ZEN_PROBE_RAY
#include "probe_common.glsl"
#define HYBRID_MIXED_PROVIDER
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
    HybridRayResult result;
#ifdef HYBRID_HARDWARE_PROVIDER
    if (ProbeFeatureHardware(feature))
    {
        result = HardwareTraceRay(HybridTraceOrigin(position, normal, 0.0), direction, 1e20, closest);
    }
    else
#endif
    {
#ifdef HYBRID_BRICK_TRAVERSAL
        // Shading hits next to a lamp are re-traced by probe_retrace_bricks.comp (probe_trace.glsl).
        result = BrickTraceRay(BrickTraceOrigin(position, normal), direction, 1e20);
#else
        result = SoftwareTraceRay(HybridTraceOrigin(position, normal, 0.0), direction, 1e20, closest);
#endif
    }
    return result;
}

// Match hit_radiance.glsl's near-light fallback and the pixel gather's bounce provider. Under brick
// traversal the trace marches bricks and the re-trace of hits next to a lamp traces opaque triangles,
// since voxels block these rays there.
bool HitLightVisible(HybridRayResult hit, SceneLight light)
{
#if defined(HYBRID_BRICK_TRAVERSAL) && !defined(PROBE_RETRACE)
    vec3 origin = BrickTraceOrigin(hit.position, hit.normal);
#else
    vec3 origin = HybridTraceOrigin(hit.position, hit.normal, 0.0);
#endif
    vec3  delta           = light.positionRange.xyz - origin;
    float distanceToLight = length(delta);
    vec3  direction       = delta / max(distanceToLight, 1e-20);
    distanceToLight       = uintBitsToFloat(floatBitsToUint(distanceToLight) - uint(distanceToLight > 0.0));
    bool blocked          = false;
    if (distanceToLight > 0.0)
    {
#ifdef HYBRID_HARDWARE_PROVIDER
        if (ProbeFeatureHardware(BOUNCE_PROVIDER))
        {
            blocked = HardwareTraceRay(origin, direction, distanceToLight, false).hit;
        }
        else
#endif
        {
#if defined(HYBRID_BRICK_TRAVERSAL) && !defined(PROBE_RETRACE)
            blocked = BrickTraceRay(origin, direction, distanceToLight).hit;
#elif defined(HYBRID_BRICK_TRAVERSAL)
            blocked = SoftwareTraceOpaqueRay(origin, direction, distanceToLight, false).hit;
#else
            blocked = SoftwareTraceRay(origin, direction, distanceToLight, false).hit;
#endif
        }
    }
    return !blocked;
}
#endif
