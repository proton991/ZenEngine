#include "Graphics/Shared/EnvironmentSampling.h"
// Pooled-guide record per tile, matching HybridGIRenderer's buffer size.
const uint POOLED_GUIDE_STRIDE = ZEN_HYBRID_GUIDE_TILES + 4u * 6u * ZEN_HYBRID_GUIDE_PER_FACE + 4u;
// Visibility guide: per screen tile, the visible residual radiance (without the receiver's cosine)
// over the environment's tiles, learned from this pass's rays, followed by the frames it averages.
// Each receiver weights it by its own cosine toward the tile centres and draws guided rays from it,
// so its rays go where the sky is both bright and unblocked from that part of the screen.
layout(std430, set = 1, binding = 10) readonly buffer HybridGuidePrevious
{
    float guidePrevious[];
};
#if defined(HYBRID_BUFFER_GUIDE) || defined(HYBRID_GUIDE_FINISH)
layout(std430, set = 1, binding = 11) buffer HybridGuideNext
{
    uint guideAccumulation[];
};
#elif !defined(HYBRID_TRACE_FALLBACK)
layout(std430, set = 1, binding = 11) writeonly buffer HybridGuideNext
{
    float guideNext[];
};
#endif
bool guideValid;
uint guideBase;
// Learned frames are averaged equally up to this count, then exponentially.
const float HYBRID_GUIDE_FRAMES = 64.0;
