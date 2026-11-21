#ifndef ZEN_NATIVE_PATH
#define ZEN_NATIVE_PATH
#include "world_cache.glsl"
#include "hybrid_common.glsl"
// Radiance of a path segment that leaves the scene.
vec3 NativeMissRadiance(vec3 direction, bool valid)
{
    return float(valid) * textureLod(coneEnvironmentMap, EnvironmentDirection(direction), 0).rgb
         * (sceneUbo.environment.x * sceneUbo.environment.z * gi.lighting.y);
}
// Shade one probe segment. Cold hits receive direct endpoint lighting.
vec3 NativePathRadiance(vec3 position, vec3 normal, vec3 direction, uint seed, out bool firstHit, out bool valid)
{
#ifdef HYBRID_FIELD_TRAVERSAL
    HybridRayResult hit   = NativeTraceRay(position, normal, direction, 1e20, true);
    vec3            value = NativeFieldRadiance(hit, seed);
#else
    rayShading            = uvec3(seed, 3u | 8u, 2u);
    HybridRayResult hit   = HardwareTraceRay(HybridTraceOrigin(position, normal, 0.0), direction, 1e20, true);
    vec3            value = hitPayload.radiance;
#endif
    firstHit = hit.hit;
    valid    = hit.valid;
    return hit.hit ? value : NativeMissRadiance(direction, valid);
}
#endif
