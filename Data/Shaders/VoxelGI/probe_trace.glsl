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
layout(std430, set = 7, binding = 1) writeonly buffer ProbeOutput
{
    ProbeRecord probeOutput[];
};

// Four SH coefficients and the anchor per lane and signal: 10 KiB plus one 208-byte record,
// below the 16 KiB portability limit. Index 256 + lane holds the anchor.
shared vec4 probeSkyPartial[320];
shared vec4 probeBouncePartial[320];
// Per lane: completed sky and bounce samples. Unavailable field samples are left out of both sums.
shared uvec2       probeValidPartial[64];
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

void main()
{
    uint lane = gl_LocalInvocationIndex;
    uint slot = gl_WorkGroupID.y * probe.grid.x + gl_WorkGroupID.x;
    if (lane == 0u)
    {
        ProbeLoadPlacement(slot);
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
        uint  batches        = uint(clamp(probeChosen.screen.w, 1.0, 4.0));
        bool  sharedProvider = ProbeFeatureHardware(SKY_PROVIDER) == ProbeFeatureHardware(BOUNCE_PROVIDER);
        vec3  origin         = probeChosen.position.xyz;
        vec3  normal         = probeChosen.normal.xyz;
        uvec2 validSamples   = uvec2(0);
        for (uint coefficient = 0u; coefficient < 5u; ++coefficient)
        {
            probeSkyPartial[coefficient * 64u + lane]    = vec4(0);
            probeBouncePartial[coefficient * 64u + lane] = vec4(0);
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
            if (skyHit.valid && !skyHit.hit)
            {
                sky = vec4(ProbeEnvironment(direction), 1);
            }
            vec4 bounce = hybrid.bounce.x != 0u ? ProbeBounce(bounceHit, direction) : vec4(0);
            vec4 basis  = ProbeSHBasis(direction) * (2.0 * HYBRID_PI / 64.0);
            // Uniform hemisphere samples: irradiance / pi = 2 * mean(L cos).
            float cosine    = 2.0 / 64.0 * max(dot(direction, normal), 0.0);
            vec2  included  = vec2(skyHit.valid ? 1.0 : 0.0, bounceHit.valid ? 1.0 : 0.0);
            validSamples   += uvec2(included);
            for (uint coefficient = 0u; coefficient < 4u; ++coefficient)
            {
                probeSkyPartial[coefficient * 64u + lane]    += sky * basis[coefficient] * included.x;
                probeBouncePartial[coefficient * 64u + lane] += bounce * basis[coefficient] * included.y;
            }
            probeSkyPartial[256u + lane]    += sky * cosine * included.x;
            probeBouncePartial[256u + lane] += bounce * cosine * included.y;
        }
        probeValidPartial[lane] = validSamples;
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
                probeValidPartial[lane] += probeValidPartial[lane + stride];
            }
            barrier();
        }
        if (lane == 0u)
        {
            // Each lane's terms are scaled for 64 samples; dividing by the completed count averages
            // the batches, and leaves unavailable samples out instead of counting them as dark.
            uvec2 valid = probeValidPartial[0];
            vec2  scale = 64.0 / max(vec2(valid), vec2(1.0));
            for (uint coefficient = 0u; coefficient < 4u; ++coefficient)
            {
                probeChosen.sky[coefficient]    = probeSkyPartial[coefficient * 64u] * scale.x;
                probeChosen.bounce[coefficient] = probeBouncePartial[coefficient * 64u] * scale.y;
            }
            probeChosen.skyAnchor    = probeSkyPartial[256u] * scale.x;
            probeChosen.bounceAnchor = probeBouncePartial[256u] * scale.y;
            if (valid.x == 0u || (hybrid.bounce.x != 0u && valid.y == 0u))
            {
                // No completed observation: publish an empty slot, never a valid black probe.
                probeChosen.position.w = 0.0;
            }
            probeOutput[slot] = probeChosen;
        }
    }
}
