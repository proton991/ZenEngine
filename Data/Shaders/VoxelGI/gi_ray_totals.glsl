#ifndef ZEN_GI_RAY_TOTALS
#define ZEN_GI_RAY_TOTALS
// Rays issued outside the hybrid trace, counted only by diagnostic variants that define
// GI_COUNT_RAY_SLOT (and HYBRID_COUNT_RAYS before the provider is included). Two totals per
// slot, Compute then HW RT; slots match VoxelGIRenderer::RayTotalSlot: 0 sky cache,
// 1 radiance-injection shadows, 2 deferred receiver shadows, 3 forward receiver shadows.
#ifdef GI_COUNT_RAY_SLOT
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
        atomicAdd(giRayTotals[2u * uint(GI_COUNT_RAY_SLOT)], compute);
    }
    if (hardware != 0u)
    {
        atomicAdd(giRayTotals[2u * uint(GI_COUNT_RAY_SLOT) + 1u], hardware);
    }
}
#endif
#endif
