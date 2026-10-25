#include "probe_common.glsl"
layout(local_size_x = 8, local_size_y = 8) in;
layout(std430, set = 7, binding = 1) writeonly buffer ProbeOutput
{
    ProbeRecord probeOutput[];
};
layout(std430, set = 7, binding = 2) readonly buffer ProbeInput
{
    ProbeRecord probeInput[];
};
layout(std430, set = 7, binding = 3) readonly buffer ProbeHistory
{
    ProbeRecord probeHistory[];
};
// probe_allocate.comp's counters: the trace wrote the uniform probes and this many adaptive ones.
layout(std430, set = 7, binding = 22) readonly buffer ProbeCounters
{
    uint  adaptiveProbes;
    uint  requestedProbes;
    uint  counterReserved[2];
    uvec4 traceGroups;
};
// The previous frame's adaptive probe per subtile, indexing ProbeHistory.
layout(std430, set = 7, binding = 24) readonly buffer ProbeSubtilesPrevious
{
    uvec2 probeSubtilesPrevious[];
};
#ifdef PROBE_RETRACED
// probe_retrace_bricks.comp's re-traced rays (probe_trace.glsl): per traced slot its last queued
// ray, chained through .z, and per ray its sky and bounce.
layout(std430, set = 7, binding = 33) readonly buffer ProbeRetraceHeads
{
    uint retraceHeads[];
};
layout(std430, set = 7, binding = 34) readonly buffer ProbeRetraceRays
{
    uvec4 retraceRays[];
};
layout(std430, set = 7, binding = 35) readonly buffer ProbeRetraceResults
{
    vec4 retraceResults[];
};
#endif

ProbeRecord previous;
float       weight;
float       footprint;
float       skyHistoryScale;

void Accumulate(ProbeRecord current, ProbeRecord old, float screenWeight)
{
    float support = ProbeSurfaceWeight(current.position.xyz, current.normal.xyz, footprint, old);
    if (floatBitsToUint(old.screen.z) == floatBitsToUint(current.screen.z) && support > 0.0)
    {
        float w = support * screenWeight;
        for (int coefficient = 0; coefficient < 4; ++coefficient)
        {
            previous.sky[coefficient]    += old.sky[coefficient] * w;
            previous.bounce[coefficient] += old.bounce[coefficient] * w;
        }
        previous.skyAnchor    += ProbeSkyIrradiance(old, current.normal.xyz) * w;
        previous.bounceAnchor += ProbeBounceIrradiance(old, current.normal.xyz) * w;
        previous.normal.w     += old.normal.w * w;
        previous.screen.w     += old.screen.w * w;
        weight                += w;
    }
}

#ifdef PROBE_RETRACED
// Adds a traced slot's re-traced rays to its record, as the trace would have: projected like its
// other rays and averaged over its batches. A slot queues at most 256 rays (64 lanes, four batches).
void AddRetracedRays(uint slot, inout ProbeRecord current)
{
    float batches = max(current.screen.w, 1.0);
    uint  ray     = retraceHeads[slot];
    for (uint count = 0u; ray != ZEN_PROBE_INVALID && count < 256u; ++count)
    {
        uvec4 entry     = retraceRays[ray];
        vec3  direction = ProbeRayDirection(current.screen, current.normal.xyz, entry.y & 63u, entry.y >> 6u);
        vec4  basis     = ProbeSHBasis(direction) * (2.0 * HYBRID_PI / 64.0) / batches;
        float cosine    = 2.0 / 64.0 * max(dot(direction, current.normal.xyz), 0.0) / batches;
        vec4  sky       = retraceResults[ray * 2u];
        vec4  bounce    = retraceResults[ray * 2u + 1u];
        for (int coefficient = 0; coefficient < 4; ++coefficient)
        {
            current.sky[coefficient]    += sky * basis[coefficient];
            current.bounce[coefficient] += bounce * basis[coefficient];
        }
        current.skyAnchor    += sky * cosine;
        current.bounceAnchor += bounce * cosine;
        ray                   = entry.z;
    }
}
#endif

void main()
{
    // Uniform probes, then adaptive slots in the extra rows.
    uvec2 coordinate = gl_GlobalInvocationID.xy;
    if (coordinate.x < probe.grid.x && coordinate.y < probe.grid.y + probe.adaptive.z)
    {
        uint        index   = coordinate.y * probe.grid.x + coordinate.x;
        ProbeRecord current = index < ProbeUniformCount() + adaptiveProbes ? probeInput[index] : ProbeEmpty();
#ifdef PROBE_RETRACED
        if (index < ProbeUniformCount() + adaptiveProbes)
        {
            AddRetracedRays(index, current);
        }
#endif
        // probe_place.comp and probe_adaptive.comp name a kept probe's history slot by a negative normal.w,
        // and screen.w holds the ray batches the trace averaged (ProbeRayBatches): history counts batches.
        uint  kept       = current.normal.w < 0.0 ? uint(-current.normal.w) - 1u : ZEN_PROBE_INVALID;
        float batches    = current.position.w > 0.5 ? max(current.screen.w, 1.0) : 1.0;
        current.normal.w = batches;
        current.screen.w = batches;
        previous         = ProbeEmpty();
        weight           = 0.0;
        skyHistoryScale  = 1.0;
        footprint        = current.position.w > 0.5 ? ProbePixelFootprint(ivec2(current.screen.xy)) : 0.0;
        if (current.position.w > 0.5 && ProbeHistoryValid())
        {
            // A kept probe is at the same surface point as last frame: it takes its own record
            // alone and keeps four times the sky history. Its 64 rays change direction every frame,
            // and a whole probe tile shifting by about 1% per frame read as water-like shimmer
            // (Sponza, 2870x1579: 3.9% of still pixels changed sky light by more than 1% per frame,
            // 0.3% with this). Blending neighbours' histories every frame instead would diffuse
            // light between probes over that long history. Environment and geometry changes
            // restart it.
            if (kept < ProbeHistorySlotCount())
            {
                Accumulate(current, probeHistory[kept], 1.0);
                skyHistoryScale = weight > 0.0 ? 4.0 : 1.0;
            }
            vec4 motion = texelFetch(receiverMotion, ivec2(current.screen.xy), 0);
            if (motion.z > 0.0 && weight == 0.0)
            {
                // A new probe starts from the previous probes around its reprojection, weighted by
                // where they were: kept probes sit anywhere in their tiles.
                vec2  previousPixel = ProbePreviousPixel(current.screen.xy);
                ivec2 tile          = ivec2(floor(previousPixel / float(probe.grid.z)));
                ivec2 subtile       = ivec2(floor(previousPixel / float(ZEN_PROBE_SUBTILE)));
                for (int y = -1; y <= 1; ++y)
                {
                    for (int x = -1; x <= 1; ++x)
                    {
                        ivec2 sampleCoordinate = tile + ivec2(x, y);
                        if (ProbeGridInside(sampleCoordinate))
                        {
                            ProbeRecord old = probeHistory[ProbeIndex(sampleCoordinate)];
                            Accumulate(current, old, ProbeScreenWeight(old.screen.xy, previousPixel, 2.0));
                        }
                        ivec2 sampleSubtile = subtile + ivec2(x, y);
                        uvec2 slots         = ProbeSubtileInside(sampleSubtile)
                                                ? probeSubtilesPrevious[ProbeSubtileIndex(sampleSubtile)]
                                                : uvec2(ZEN_PROBE_INVALID);
                        for (uint entry = 0u; entry < ZEN_PROBE_ADAPTIVE_PER_SUBTILE; ++entry)
                        {
                            if (slots[entry] < ProbeHistorySlotCount())
                            {
                                ProbeRecord old = probeHistory[slots[entry]];
                                Accumulate(current, old, ProbeScreenWeight(old.screen.xy, previousPixel, 2.0));
                            }
                        }
                    }
                }
            }
        }
        if (weight > 0.01)
        {
            float limit      = max(probe.parameters.y, 1.0);
            current.normal.w = min(previous.normal.w / weight + batches, limit * skyHistoryScale);
            // Bounce keeps its history when the radiance cache changed by one step (a light moving
            // every frame) instead of restarting: probes cannot reread the previous cache along the
            // same rays as the pixel gather does. A frame with a change holds at most four frames and
            // the responsive window eight, trading a few frames of lag for an estimate that converges.
            // Only an incomparable cache (bounce.y == 0) restarts it.
            bool  bounceHistory = hybrid.bounce.y != 0u;
            float bounceLimit   = hybrid.bounce.w != 0u                             ? min(limit, 4.0)
                                : probe.parameters.z > 0.5 || hybrid.bounce.z != 0u ? min(limit, 8.0)
                                                                                    : limit;
            current.screen.w    = bounceHistory ? min(previous.screen.w / weight + batches, bounceLimit) : batches;
            float skyBlend      = min(batches / current.normal.w, 1.0);
            float bounceBlend   = min(batches / current.screen.w, 1.0);
            for (int coefficient = 0; coefficient < 4; ++coefficient)
            {
                current.sky[coefficient] = mix(previous.sky[coefficient] / weight, current.sky[coefficient], skyBlend);
                if (bounceHistory)
                {
                    current.bounce[coefficient] =
                        mix(previous.bounce[coefficient] / weight, current.bounce[coefficient], bounceBlend);
                }
            }
            current.skyAnchor = mix(previous.skyAnchor / weight, current.skyAnchor, skyBlend);
            if (bounceHistory)
            {
                current.bounceAnchor = mix(previous.bounceAnchor / weight, current.bounceAnchor, bounceBlend);
            }
        }
        probeOutput[index] = current;
    }
}
