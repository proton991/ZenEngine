#include "probe_common.glsl"
#include "probe_ray.glsl"
layout(local_size_x = 64) in;
// Uniform placements (probe_place.comp), then the frame's adaptive ones (probe_adaptive.comp).
layout(std430, set = 7, binding = 21) readonly buffer ProbePlacements
{
    ProbePlacement probePlacements[];
};
layout(std430, set = 7, binding = 25) readonly buffer ProbeAdaptivePlacements
{
    ProbePlacement probeAdaptivePlacements[];
};
// probe_allocate.comp's counters; traceGroups dispatched this pass.
layout(std430, set = 7, binding = 22) readonly buffer ProbeCounters
{
    uint  adaptiveProbes;
    uint  requestedProbes;
    uint  counterReserved[2];
    uvec4 traceGroups;
};
#ifdef HYBRID_BRICK_TRAVERSAL
// Shading rays whose brick hit lies next to a cached point or spot light (HitNearLocalLight) are
// queued: the brick trace leaves them out and probe_retrace_bricks.comp re-traces them against
// triangles, one thread per ray, so brick trace shaders carry no triangle traversal.
// Header: x the re-trace's dispatch groups, w the rays claimed (claims past the capacity, slots
// times ZEN_PROBE_RETRACE_RAYS_PER_SLOT, are shaded from their brick hit instead).
#    ifdef PROBE_RETRACE
#        define PROBE_RETRACE_QUEUE_ACCESS readonly
#        define PROBE_RETRACE_RAYS_ACCESS  readonly
#    else
#        define PROBE_RETRACE_QUEUE_ACCESS
#        define PROBE_RETRACE_RAYS_ACCESS writeonly
#    endif
layout(std430, set = 7, binding = 32) PROBE_RETRACE_QUEUE_ACCESS buffer ProbeRetraceHeader
{
    uvec4 retraceHeader;
};
// Per ray: slot, lane | batch << 6, the slot's next queued ray, brick hit distance bits.
layout(std430, set = 7, binding = 34) PROBE_RETRACE_RAYS_ACCESS buffer ProbeRetraceRays
{
    uvec4 retraceRays[];
};
#    ifndef PROBE_RETRACE
// Per traced slot: its last queued ray, ZEN_PROBE_INVALID without one; rays chain through .z. The
// workgroup builds its slot's chain in shared memory and writes the head once.
layout(std430, set = 7, binding = 33) writeonly buffer ProbeRetraceHeads
{
    uint retraceHeads[];
};
shared uint probeRetraceHead;
#    endif
#endif
#ifndef PROBE_RETRACE
layout(std430, set = 7, binding = 1) writeonly buffer ProbeOutput
{
    ProbeRecord probeOutput[];
};
#else
// Per queued ray: sky, then bounce (probe_temporal.comp adds them to the ray's probe).
layout(std430, set = 7, binding = 35) writeonly buffer ProbeRetraceResults
{
    vec4 retraceResults[];
};
#endif

// Four SH coefficients and the anchor per lane and signal: 10 KiB plus one 208-byte record,
// below the 16 KiB portability limit. Index 256 + lane holds the anchor.
shared vec4        probeSkyPartial[320];
shared vec4        probeBouncePartial[320];
shared ProbeRecord probeChosen;

// A slot's placement: uniform slots first, then the frame's adaptive ones; empty past them.
ProbePlacement ProbeSlotPlacement(uint slot)
{
    ProbePlacement result = ProbePlacement(vec4(0), vec4(0), vec4(0));
    uint           count  = ProbeUniformCount();
    if (slot < count + adaptiveProbes)
    {
        result = slot < count ? probePlacements[slot] : probeAdaptivePlacements[slot - count];
    }
    return result;
}

// One workgroup per probe slot, up to the last allocated adaptive row. Adaptive slots past this
// frame's count stay empty.
void ProbeLoadPlacement(uint slot)
{
    ProbePlacement placement = ProbeSlotPlacement(slot);
    probeChosen              = ProbeEmpty();
    probeChosen.position     = placement.position;
    probeChosen.normal       = placement.normal;
    probeChosen.screen       = placement.screen;
}

// Unfiltered environment radiance, as the pixel gather reads it. The prefiltered GGX mips are a
// specular split-sum filter, not an energy-preserving cone average: reading the mip matching a
// ray's 2*pi/64 steradian footprint brightened open-sky irradiance by 10-30% (curved-scene editor
// test, Papermill). Probe temporal and spatial accumulation absorb the extra variance.
vec3 ProbeEnvironment(vec3 direction)
{
    float scale = sceneUbo.environment.x * sceneUbo.environment.z * gi.lighting.y;
    return textureLod(coneEnvironmentMap, EnvironmentDirection(direction), 0.0).rgb * scale;
}

// A shading hit's bounce: RGB hit radiance, alpha one for a rejected lookup.
vec4 ProbeBounce(HybridRayResult hit, vec3 direction)
{
    vec4 bounce = vec4(0);
    if (hit.hit)
    {
        bool rejected;
        bounce.rgb = HitRadiance(hitRadiance, hitNormal, hit, direction, false, rejected);
        bounce.a   = rejected ? 1.0 : 0.0;
    }
    return bounce;
}

#ifndef PROBE_RETRACE
void main()
{
    uint lane = gl_LocalInvocationIndex;
    uint slot = gl_WorkGroupID.y * probe.grid.x + gl_WorkGroupID.x;
    if (lane == 0u)
    {
        ProbeLoadPlacement(slot);
#    ifdef HYBRID_BRICK_TRAVERSAL
        probeRetraceHead = ZEN_PROBE_INVALID;
#    endif
    }
    barrier();
    // The shared record makes this branch uniform across the workgroup, so its barriers are valid.
    // An empty slot (tile without receivers, unused adaptive slot) skips the trace and reduction.
    if (probeChosen.position.w < 0.5)
    {
        if (lane == 0u)
        {
            probeOutput[slot] = probeChosen;
        }
    }
    else
    {
        // The placement's ray batches (ProbeRayBatches): the sample averages them, and the temporal
        // pass weighs it as that many frames.
        uint batches        = uint(clamp(probeChosen.screen.w, 1.0, 4.0));
        bool sharedProvider = ProbeFeatureHardware(SKY_PROVIDER) == ProbeFeatureHardware(BOUNCE_PROVIDER);
        vec3 origin         = probeChosen.position.xyz;
        vec3 normal         = probeChosen.normal.xyz;
        vec4 skySum[5], bounceSum[5];
        for (uint coefficient = 0u; coefficient < 5u; ++coefficient)
        {
            skySum[coefficient]    = vec4(0);
            bounceSum[coefficient] = vec4(0);
        }
        for (uint batch = 0u; batch < batches; ++batch)
        {
            vec4            sky       = vec4(0);
            vec3            direction = ProbeRayDirection(probeChosen.screen, normal, lane, batch);
            HybridRayResult skyHit =
                ProbeTraceFeature(SKY_PROVIDER, origin, normal, direction, hybrid.bounce.x != 0u && sharedProvider);
            HybridRayResult bounceHit = skyHit;
            if (hybrid.bounce.x != 0u && !sharedProvider)
            {
                bounceHit = ProbeTraceFeature(BOUNCE_PROVIDER, origin, normal, direction, true);
            }
            bool queued = false;
#    ifdef HYBRID_BRICK_TRAVERSAL
            // A shading ray that hit bricks next to a lamp: its bounce, and its sky when the sky ray
            // is the shading ray, come from probe_retrace_bricks.comp.
            if (hybrid.bounce.x != 0u && !ProbeFeatureHardware(sharedProvider ? SKY_PROVIDER : BOUNCE_PROVIDER) && bounceHit.hit
                && HitNearLocalLight(bounceHit.position))
            {
                uint ray = atomicAdd(retraceHeader.w, 1u);
                queued   = ray < ProbeSlotCount() * ZEN_PROBE_RETRACE_RAYS_PER_SLOT;
                if (queued)
                {
                    // Rays claim queue entries in order, so the first of every 64 adds a group.
                    if (ray % 64u == 0u)
                    {
                        atomicAdd(retraceHeader.x, 1u);
                    }
                    float distance = length(bounceHit.position - BrickTraceOrigin(origin, normal));
                    retraceRays[ray] =
                        uvec4(slot, lane | batch << 6u, atomicExchange(probeRetraceHead, ray), floatBitsToUint(distance));
                }
            }
#    endif
            if (!skyHit.hit && !(queued && sharedProvider))
            {
                sky = vec4(ProbeEnvironment(direction), 1);
            }
            vec4 bounce = hybrid.bounce.x != 0u && !queued ? ProbeBounce(bounceHit, direction) : vec4(0);
            vec4 basis  = ProbeSHBasis(direction) * (2.0 * HYBRID_PI / 64.0);
            // Uniform hemisphere samples: irradiance / pi = 2 * mean(L cos).
            float cosine = 2.0 / 64.0 * max(dot(direction, normal), 0.0);
            for (uint coefficient = 0u; coefficient < 4u; ++coefficient)
            {
                skySum[coefficient]    += sky * basis[coefficient];
                bounceSum[coefficient] += bounce * basis[coefficient];
            }
            skySum[4]    += sky * cosine;
            bounceSum[4] += bounce * cosine;
        }
        for (uint coefficient = 0u; coefficient < 5u; ++coefficient)
        {
            uint index                = coefficient * 64u + lane;
            probeSkyPartial[index]    = skySum[coefficient] / float(batches);
            probeBouncePartial[index] = bounceSum[coefficient] / float(batches);
        }
        barrier();
        for (uint stride = 32u; stride > 0u; stride >>= 1u)
        {
            if (lane < stride)
            {
                for (uint coefficient = 0u; coefficient < 5u; ++coefficient)
                {
                    uint index                 = coefficient * 64u + lane;
                    probeSkyPartial[index]    += probeSkyPartial[index + stride];
                    probeBouncePartial[index] += probeBouncePartial[index + stride];
                }
            }
            barrier();
        }
        if (lane == 0u)
        {
            for (uint coefficient = 0u; coefficient < 4u; ++coefficient)
            {
                probeChosen.sky[coefficient]    = probeSkyPartial[coefficient * 64u];
                probeChosen.bounce[coefficient] = probeBouncePartial[coefficient * 64u];
            }
            probeChosen.skyAnchor    = probeSkyPartial[256u];
            probeChosen.bounceAnchor = probeBouncePartial[256u];
            probeOutput[slot]        = probeChosen;
        }
    }
#    ifdef HYBRID_BRICK_TRAVERSAL
    // After the reduction's barriers, every lane has queued its rays; empty slots write none.
    if (lane == 0u)
    {
        retraceHeads[slot] = probeRetraceHead;
    }
#    endif
}
#else
// One thread per queued ray: the trace's direction and brick hit, re-traced from one fine voxel
// before the hit (BrickRetraceTriangles), and shaded with triangle visibility of near lights.
void main()
{
    uint ray = gl_GlobalInvocationID.x;
    if (ray < min(retraceHeader.w, ProbeSlotCount() * ZEN_PROBE_RETRACE_RAYS_PER_SLOT))
    {
        uvec4          entry     = retraceRays[ray];
        ProbePlacement placement = ProbeSlotPlacement(entry.x);
        vec3           normal    = placement.normal.xyz;
        vec3           direction = ProbeRayDirection(placement.screen, normal, entry.y & 63u, entry.y >> 6u);
        vec3 start = BrickTraceOrigin(placement.position.xyz, normal) + direction * (uintBitsToFloat(entry.w) - brick.voxel.x);
        HybridRayResult hit            = BrickRetraceTriangles(start, direction);
        bool            sharedProvider = ProbeFeatureHardware(SKY_PROVIDER) == ProbeFeatureHardware(BOUNCE_PROVIDER);
        retraceResults[ray * 2u]       = sharedProvider && !hit.hit ? vec4(ProbeEnvironment(direction), 1) : vec4(0);
        retraceResults[ray * 2u + 1u]  = ProbeBounce(hit, direction);
    }
}
#endif
