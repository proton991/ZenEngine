#pragma once

#include "Graphics/RHI/RHICommon.h"
#include "Graphics/RenderCore/V2/RenderGraph/RDGDefs.h"

namespace zen::rc
{
class RenderDevice;
class RenderGraph;

constexpr uint32_t kForwardSortGroupSize = 256;
constexpr uint32_t kForwardSortMaxGroups = 8192;
constexpr uint32_t kForwardSortMaxKeys   = kForwardSortGroupSize * kForwardSortMaxGroups;

bool SupportsForwardDepthSort(const RHIGPUInfo& gpu, uint32_t count);

// Stable ascending uint32 depth keys with uint32 triangle identities. The caller
// produces all input keys before this pass. Eight 4-bit passes finish in input;
// failure declares no sorting passes and returns an empty handle.
RDGBuffer AddForwardDepthSort(RenderDevice& device, RenderGraph& graph, RDGBuffer input, uint32_t count);
} // namespace zen::rc
