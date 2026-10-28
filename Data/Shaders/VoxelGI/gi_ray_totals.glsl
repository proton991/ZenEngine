#ifndef ZEN_GI_RAY_TOTALS
#define ZEN_GI_RAY_TOTALS
// Rays issued outside the hybrid trace, counted only by diagnostic variants that define
// GI_COUNT_RAY_SLOT (and HYBRID_COUNT_RAYS before the provider is included). Six totals per
// slot, Compute/HW rays and four field work counters; slots match VoxelGIRenderer::RayTotalSlot: 0 sky cache,
// 1 radiance-injection shadows, 2 deferred receiver shadows, 3 forward receiver shadows.
#ifdef GI_COUNT_RAY_SLOT
#    include "Graphics/Shared/VoxelGI.h"
#    include "hybrid_ray_counts.glsl"
layout(std430, set = 6, binding = 9) buffer GIRayTotals
{
    uint giRayTotals[];
};
void RecordGIRays()
{
    uint compute  = issuedComputeRays.x + issuedComputeRays.y + issuedComputeRays.z + issuedComputeRays.w;
    uint hardware = issuedHardwareRays.x + issuedHardwareRays.y + issuedHardwareRays.z + issuedHardwareRays.w;
    if (compute != 0u)
    {
        atomicAdd(giRayTotals[ZEN_GI_TOTAL_COUNT_WORDS * uint(GI_COUNT_RAY_SLOT)], compute);
    }
    if (hardware != 0u)
    {
        atomicAdd(giRayTotals[ZEN_GI_TOTAL_COUNT_WORDS * uint(GI_COUNT_RAY_SLOT) + 1u], hardware);
    }
    for (uint counter = 0u; counter < 4u; ++counter)
    {
        if (issuedFieldWork[counter] != 0u)
        {
            atomicAdd(giRayTotals[ZEN_GI_TOTAL_COUNT_WORDS * uint(GI_COUNT_RAY_SLOT) + 2u + counter], issuedFieldWork[counter]);
        }
    }
}
#endif
#endif
