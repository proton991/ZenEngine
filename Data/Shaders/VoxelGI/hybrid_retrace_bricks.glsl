// Re-traces the diffuse rays a brick trace pass queued because their hit lies next to a lamp
// (hybrid_retrace.glsl) against triangles, one thread per listed receiver, and adds them to the
// images that pass wrote: the half-resolution ones before the upsample, otherwise the raw ones.
// With HYBRID_COUNT_RAYS the rays it issues are added to the ray-count capture as bounce rays.
#define HYBRID_BRICK_TRAVERSAL
#define HYBRID_RETRACE_PASS
#include "hybrid_guides.glsl"
#include "hybrid_provider.glsl"
#include "hybrid_retrace.glsl"
layout(local_size_x = 64) in;
// The trace pass's bindings of these inputs.
layout(set = 1, binding = 6) uniform sampler3D voxelOpacity;
layout(set = 1, binding = 7) uniform samplerCube coneEnvironmentMap;
layout(set = 1, binding = 13) uniform sampler3D hitRadiance;
layout(set = 1, binding = 14) uniform sampler3D hitNormal;
layout(set = 1, binding = 15) uniform sampler3D hitRadiancePrevious;
#define HIT_LIGHT_CACHE
#include "hit_radiance.glsl"
layout(set = 1, binding = 44, rgba16f) uniform image2D retraceSky;
layout(set = 1, binding = 45, rgba16f) uniform image2D retraceBounce;
layout(set = 1, binding = 46, rgba16f) uniform image2D retraceBouncePrevious;
#ifdef HYBRID_COUNT_RAYS
// The trace pass's counters. These rays are added to the first tile's fallback entries, which the
// trace pass cleared, as the fallback pass adds its own (hybrid_trace.glsl).
layout(std430, set = 1, binding = 26) buffer HybridRayCounts
{
    uvec4 rayCounts[];
};
#endif

// Feature bits and the bounce ray-count slot as in hybrid_trace.glsl.
const uint SKY_PROVIDER    = 1u;
const uint BOUNCE_PROVIDER = 8u;
const uint BOUNCE_RAYS     = 2u;

// Near-light visibility from a re-traced hit, against opaque triangles: these hits lie centimetres
// from a lamp, where voxels block what triangles let through.
bool HitLightVisible(HybridRayResult hit, SceneLight light)
{
#ifdef HYBRID_COUNT_RAYS
    issuedRayFeature = BOUNCE_RAYS;
#endif
    vec3  origin          = HybridTraceOrigin(hit.position, hit.normal, 0.0);
    vec3  delta           = light.positionRange.xyz - origin;
    float distanceToLight = length(delta);
    vec3  direction       = delta / max(distanceToLight, 1e-20);
    // Exclude the endpoint itself, without dropping a finite interval before it.
    distanceToLight = uintBitsToFloat(floatBitsToUint(distanceToLight) - uint(distanceToLight > 0.0));
    return distanceToLight <= 0.0 || !SoftwareTraceOpaqueRay(origin, direction, distanceToLight, false).hit;
}

void main()
{
    uint index = gl_GlobalInvocationID.x;
    if (index < min(retraceHeader.receivers, uint(retraceReceivers.length())))
    {
        uvec2 receiver = retraceReceivers[index];
        ivec2 target   = ivec2(receiver.x & 0xffffu, receiver.x >> 16u);
        // When the sky ray is the bounce ray, a re-trace that escapes adds sky, as in the trace.
        bool  sharedBounce = ((hybrid.provider.z & SKY_PROVIDER) != 0u) == ((hybrid.provider.z & BOUNCE_PROVIDER) != 0u);
        float scale        = sceneUbo.environment.x * sceneUbo.environment.z * gi.lighting.y;
        vec4  sky = vec4(0), bounce = vec4(0), previous = vec4(0);
        uint  ray = receiver.y;
        for (uint walked = 0u; HybridRetraceContinues(ray, walked); ++walked)
        {
            uvec4 start     = retraceRays[ray * 2u];
            uvec4 entry     = retraceRays[ray * 2u + 1u];
            vec3  direction = uintBitsToFloat(entry.xyz);
            float weight    = uintBitsToFloat(entry.w);
#ifdef HYBRID_COUNT_RAYS
            issuedRayFeature = BOUNCE_RAYS;
#endif
            HybridRayResult hit = BrickRetraceTriangles(uintBitsToFloat(start.xyz), direction);
            if (hit.hit)
            {
                bool rejected;
                bounce.rgb += HitRadiance(hitRadiance, hitNormal, hit, direction, false, rejected) * abs(weight);
                bounce.a   += rejected ? 1.0 : 0.0;
                if (hybrid.bounce.w != 0u)
                {
                    previous.rgb += HitRadiance(hitRadiancePrevious, hitNormal, hit, direction, true, rejected) * abs(weight);
                }
            }
            else if (sharedBounce)
            {
                // Environment source directions are integrated by their own rays; they add only
                // to the visible fraction in alpha.
                if (weight > 0.0)
                {
                    sky.rgb += textureLod(coneEnvironmentMap, EnvironmentDirection(direction), 0).rgb * weight * scale;
                }
                sky.a += abs(weight);
            }
            ray = start.w;
        }
        imageStore(retraceSky, target, imageLoad(retraceSky, target) + sky);
        imageStore(retraceBounce, target, imageLoad(retraceBounce, target) + bounce);
        imageStore(retraceBouncePrevious, target, imageLoad(retraceBouncePrevious, target) + previous);
#ifdef HYBRID_COUNT_RAYS
        uvec2 size = uvec2(textureSize(receiverDepth, 0));
        uint  slot = (((size.y + 15u) / 16u) * ((size.x + 15u) / 16u)) * 2u;
        if (issuedComputeRays[BOUNCE_RAYS] != 0u)
        {
            atomicAdd(rayCounts[slot][BOUNCE_RAYS], issuedComputeRays[BOUNCE_RAYS]);
        }
#endif
    }
}
