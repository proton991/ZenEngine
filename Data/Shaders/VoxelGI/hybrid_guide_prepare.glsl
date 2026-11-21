#include "gi_common.glsl"
#include "hybrid_uniforms.glsl"
#include "environment_sampling.glsl"
layout(local_size_x = 16, local_size_y = 16) in;
const uint TRACE_THREADS = 256u;
#include "hybrid_guide_storage.glsl"
#include "hybrid_guide_build.glsl"
layout(std430, set = 1, binding = 37) writeonly buffer HybridGuidePooled
{
    float guidePooled[];
};
layout(std430, set = 1, binding = 55) writeonly buffer HybridGuideLearning
{
    uint guideLearningWords[];
};
#ifdef HYBRID_COUNT_RAYS
#    include "Graphics/Shared/VoxelGI.h"
layout(std430, set = 1, binding = 26) writeonly buffer HybridRayCounts
{
    uvec4 rayCounts[];
};
#endif
void main()
{
    uint lane = gl_LocalInvocationIndex;
    BuildGuide(lane);
    uint tile = gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
    uint base = tile * ZEN_HYBRID_GUIDE_POOLED_FLOATS;
    for (uint t = lane; t < ZEN_HYBRID_GUIDE_TILES; t += TRACE_THREADS)
    {
        guidePooled[base + t] = guideWeight[t];
    }
    if (lane < 6u * ZEN_HYBRID_GUIDE_PER_FACE)
    {
        for (uint component = 0u; component < 4u; ++component)
        {
            guidePooled[base + ZEN_HYBRID_GUIDE_TILES + 4u * lane + component] = guideRowMoment[lane][component];
        }
    }
    if (lane == 0u)
    {
        guidePooled[base + ZEN_HYBRID_GUIDE_POOLED_FLOATS - 1u] = guidePower;
    }
    uint words = (ZEN_HYBRID_GUIDE_TILES + 1u) * HYBRID_GUIDE_WORDS;
    for (uint word = lane; word < words; word += TRACE_THREADS)
    {
        guideLearningWords[tile * words + word] = 0u;
    }
#ifdef HYBRID_COUNT_RAYS
    if (lane < ZEN_GI_TRACE_COUNT_WORDS / 4u)
    {
        rayCounts[tile * (ZEN_GI_TRACE_COUNT_WORDS / 4u) + lane] = uvec4(0);
    }
#endif
}
