#ifndef ZEN_HARDWARE_RECEIVER_LIGHTING
#define ZEN_HARDWARE_RECEIVER_LIGHTING
// Set 3 belongs to reconstructed primary lighting in forward materials.
#define ENVIRONMENT_SAMPLING_SET 7
#include "ray_hit_lighting.glsl"

// Forward layers that cannot reuse an exact opaque receiver trace their own transport.
// No screen-probe interpolation, voxel-cone fallback or cache radiance participates.
void HardwareReceiverDiffuse(vec3 position, vec3 normal, vec3 geometric, uvec2 pixel, uint count, out vec3 sky, out vec3 bounce)
{
    sky          = vec3(0);
    bounce       = vec3(0);
    uint  frame  = floatBitsToUint(gi.indirect.w);
    float scale  = sceneUbo.environment.x * sceneUbo.environment.z * gi.lighting.y;
    vec3  origin = HybridTraceOrigin(position, geometric, 0.0);
    for (uint i = 0u; i < count; ++i)
    {
        vec2  u           = HybridSample(pixel, frame * count + i, 0xf04du);
        bool  environment = HybridEnvironmentProposal(i, count, u);
        float pdf;
        vec3  direction = HybridSkySample(normal, u, environment, pdf);
        if (dot(geometric, direction) > 0.0 && dot(normal, direction) > 0.0 && pdf > 0.0)
        {
#ifdef HYBRID_COUNT_RAYS
            issuedRayFeature = 2u;
#endif
            HybridRayResult hit    = HardwareTraceRay(origin, direction, 1e20, true);
            float           weight = max(dot(normal, direction), 0.0) / (HYBRID_PI * pdf * float(count));
            if (hit.hit)
            {
                uint seed  = HybridHash(HybridMorton(pixel) ^ HybridHash(frame * count + i) ^ 0xf04db01cu);
                bounce    += HardwareHitRadiance(hit, seed) * weight;
            }
            else
            {
                sky += textureLod(coneEnvironmentMap, EnvironmentDirection(direction), 0).rgb * (scale * weight);
            }
        }
    }
}

vec4 HardwareReceiverSpecular(vec3 position, vec3 normal, vec3 geometric, vec3 view, float roughness, uvec2 pixel)
{
    vec4 result = vec4(0);
    uint mask   = uint(gi.indirect.y);
    uint frame  = floatBitsToUint(gi.indirect.w);
    uint count  = gi.indirect.z > 4.0 ? uint(gi.indirect.z) : 1u;
    vec3 origin = HybridTraceOrigin(position, geometric, 0.0);
    for (uint i = 0u; i < count; ++i)
    {
        vec3 direction = HybridSpecularDirection(normal, view, clamp(roughness, 0.04, 1.0), pixel, frame * count + i, 0xf05du);
        if (dot(geometric, direction) > 0.0)
        {
#ifdef HYBRID_COUNT_RAYS
            issuedRayFeature = 3u;
#endif
            HybridRayResult hit = HardwareTraceRay(origin, direction, 1e20, (mask & 16u) != 0u);
            if (!hit.hit)
            {
                result.a += 1.0 / float(count);
            }
            else if ((mask & 16u) != 0u)
            {
                uint seed   = HybridHash(HybridMorton(pixel) ^ HybridHash(frame * count + i) ^ 0xf05db01cu);
                result.rgb += HardwareHitRadiance(hit, seed) / float(count);
            }
        }
    }
    result.a = (mask & 2u) != 0u ? result.a : 1.0;
    return result;
}
#endif
