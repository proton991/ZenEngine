#include "hybrid_guide_history.glsl"
#if !defined(HYBRID_NO_GUIDE)
// Capture-only specialization. Normal rendering retains one float word per bin.
layout(constant_id = 0) const bool HYBRID_EXACT_GUIDE = false;
const uint HYBRID_GUIDE_WORDS                         = HYBRID_EXACT_GUIDE ? 10u : 1u;
vec3       GuideCellDirection(uint t)
{
    uint perFace = ZEN_HYBRID_GUIDE_PER_FACE * ZEN_HYBRID_GUIDE_PER_FACE;
    vec2 center  = (vec2(t % ZEN_HYBRID_GUIDE_PER_FACE, (t % perFace) / ZEN_HYBRID_GUIDE_PER_FACE) + 0.5)
                     * (2.0 / float(ZEN_HYBRID_GUIDE_PER_FACE))
                 - 1.0;
    return EnvironmentWorldDirection(t / perFace, center);
}
#    ifdef HYBRID_RAY_PIPELINE
layout(std430, set = 1, binding = 37) readonly buffer HybridGuidePooled
{
    float guidePooled[];
};
layout(std430, set = 1, binding = 55) buffer HybridGuideLearning
{
    uint guideSumWords[];
};
#        define GUIDE_SUM_EXTERNAL_STORAGE
#        include "guide_sum.glsl"
uint GuidePooledBase()
{
    return guideBase / (ZEN_HYBRID_GUIDE_TILES + 1u) * ZEN_HYBRID_GUIDE_POOLED_FLOATS;
}
vec3 GuideDirection(uint t)
{
    return GuideCellDirection(t);
}
float GuideWeightAt(uint t)
{
    return guidePooled[GuidePooledBase() + t];
}
vec4 GuideRowMomentAt(uint row)
{
    uint base = GuidePooledBase() + ZEN_HYBRID_GUIDE_TILES + 4u * row;
    return vec4(guidePooled[base], guidePooled[base + 1u], guidePooled[base + 2u], guidePooled[base + 3u]);
}
float GuidePowerValue()
{
    return guidePooled[GuidePooledBase() + ZEN_HYBRID_GUIDE_POOLED_FLOATS - 1u];
}
#    else
shared float guideWeight[ZEN_HYBRID_GUIDE_TILES];
shared vec3  guideDirection[ZEN_HYBRID_GUIDE_TILES];
vec3         GuideDirection(uint t)
{
    return guideDirection[t];
}
#        define GUIDE_SUM_BINS          ZEN_HYBRID_GUIDE_TILES
#        define GUIDE_SUM_STORAGE_WORDS HYBRID_GUIDE_WORDS
#        include "guide_sum.glsl"
shared uint  guideReceivers;
shared float guidePower;
shared float guidePartialPower[TRACE_THREADS];
shared vec4  guideRowMoment[6u * ZEN_HYBRID_GUIDE_PER_FACE];
// Guide reads use the workgroup's shared copy.
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
