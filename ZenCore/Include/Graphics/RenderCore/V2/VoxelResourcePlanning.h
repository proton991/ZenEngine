#pragma once
#include "Graphics/RHI/RHICommon.h"

namespace zen::rc
{
enum class GIResourceStatus
{
    eSuccess,
    eInvalidInput,
    eOverflow,
    eBufferSize,
    eDescriptorRange,
    eBudget
};

GIResourceStatus ValidateGIStorageBuffer(uint64_t elements,
                                         uint32_t stride,
                                         const RHIGPUInfo& gpu,
                                         uint64_t& bytes);

// Incremental scratch and output storage; zero for an unsupported resolution.
uint64_t GetVoxelReflectanceRequiredBytes(uint32_t resolution);

// Incremental V1 budget: one uint4 scratch and one RGBA8 output per cell,
// including any previous V1 allocations still awaiting retirement.
GIResourceStatus ValidateVoxelReflectanceResources(uint32_t resolution,
                                                   uint64_t triangles,
                                                   uint64_t budgetBytes,
                                                   uint64_t retiringBytes,
                                                   const RHIGPUInfo& gpu,
                                                   uint64_t& peakBytes);

} // namespace zen::rc
