#if defined(HYBRID_TRACE_HALF) || defined(HYBRID_GUIDE_PREPARE)
// Each tile's pooled guide for traces: cell weights, row moments and total power.
layout(std430, set = 1, binding = 37) writeonly buffer HybridPooledGuide
{
    float pooledGuide[];
};
#endif
#if defined(HYBRID_TRACE_FALLBACK) || defined(HYBRID_BUFFER_GUIDE)
layout(std430, set = 1, binding = 37) readonly buffer HybridPooledGuide
{
    float pooledGuide[];
};
#endif
#include "hybrid_guide_history.glsl"
#ifndef HYBRID_NO_GUIDE
#    ifndef HYBRID_BUFFER_GUIDE
shared float guideWeight[ZEN_HYBRID_GUIDE_TILES];
#    endif
vec3 GuideCellDirection(uint t)
{
    uint perFace = ZEN_HYBRID_GUIDE_PER_FACE * ZEN_HYBRID_GUIDE_PER_FACE;
    vec2 center  = (vec2(t % ZEN_HYBRID_GUIDE_PER_FACE, (t % perFace) / ZEN_HYBRID_GUIDE_PER_FACE) + 0.5)
                     * (2.0 / float(ZEN_HYBRID_GUIDE_PER_FACE))
                 - 1.0;
    return EnvironmentWorldDirection(t / perFace, center);
}
#    if defined(HYBRID_TRACE_HALF) || defined(HYBRID_TRACE_FALLBACK) || defined(HYBRID_BUFFER_GUIDE)
// A quarter as many threads share each tile's guide here. Recomputing the cell directions keeps
// the workgroup's shared memory small enough not to limit occupancy.
vec3 GuideDirection(uint t)
{
    return GuideCellDirection(t);
}
#    else
shared vec3 guideDirection[ZEN_HYBRID_GUIDE_TILES];
vec3        GuideDirection(uint t)
{
    return guideDirection[t];
}
#    endif
#    define GUIDE_SUM_BINS ZEN_HYBRID_GUIDE_TILES
// Capture-only specialization. Normal rendering retains one float word per bin.
layout(constant_id = 0) const bool HYBRID_EXACT_GUIDE = false;
const uint HYBRID_GUIDE_WORDS                         = HYBRID_EXACT_GUIDE ? 10u : 1u;
#    define GUIDE_SUM_STORAGE_WORDS HYBRID_GUIDE_WORDS
#    ifndef HYBRID_BUFFER_GUIDE
#        include "guide_sum.glsl"
shared uint  guideReceivers;
shared float guidePower;
shared float guidePartialPower[TRACE_THREADS];
shared vec4  guideRowMoment[6u * ZEN_HYBRID_GUIDE_PER_FACE];
#    endif
// Guide reads: the workgroup's shared copy, or the tile's published copy in the fallback pass.
#    if defined(HYBRID_TRACE_FALLBACK) || defined(HYBRID_BUFFER_GUIDE)
uint  pooledBase;
float GuideWeightAt(uint t)
{
    return pooledGuide[pooledBase + t];
}
vec4 GuideRowMomentAt(uint row)
{
    uint base = pooledBase + ZEN_HYBRID_GUIDE_TILES + row * 4u;
    return vec4(pooledGuide[base], pooledGuide[base + 1u], pooledGuide[base + 2u], pooledGuide[base + 3u]);
}
float GuidePowerValue()
{
    return pooledGuide[pooledBase + ZEN_HYBRID_GUIDE_TILES + 4u * 6u * ZEN_HYBRID_GUIDE_PER_FACE];
}
#    else
float GuideWeightAt(uint t)
{
    return guideWeight[t];
}
vec4 GuideRowMomentAt(uint row)
{
    return guideRowMoment[row];
}
float GuidePowerValue()
{
    return guidePower;
}
#    endif
#endif
