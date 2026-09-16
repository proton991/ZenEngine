#pragma once
#include "VulkanHeaders.h"

namespace zen
{
// RHI resources can be used on graphics and compute without queue-family ownership transfers.
// Transfer usage additionally permits the dedicated transfer queue. Synchronization between
// accesses on different queues is still the caller's responsibility.
// The allocator consumes create-info synchronously while its queue-family storage is alive.
// Clear the borrowed pointer before returning so cached create-info cannot retain stack storage.
template <typename CreateInfo, typename Allocate>
void AllocateWithQueueSharing(CreateInfo& info,
                              uint32_t graphicsFamily,
                              uint32_t computeFamily,
                              uint32_t transferFamily,
                              bool transferUsage,
                              Allocate&& allocate)
{
    uint32_t families[3] = {graphicsFamily};
    uint32_t familyCount = 1;
    if (computeFamily != graphicsFamily)
    {
        families[familyCount++] = computeFamily;
    }
    if (transferUsage && transferFamily != graphicsFamily && transferFamily != computeFamily)
    {
        families[familyCount++] = transferFamily;
    }
    const bool concurrent = familyCount > 1;
    info.sharingMode      = concurrent ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
    info.queueFamilyIndexCount = concurrent ? familyCount : 0;
    info.pQueueFamilyIndices   = concurrent ? families : nullptr;
    allocate();
    info.queueFamilyIndexCount = 0;
    info.pQueueFamilyIndices   = nullptr;
}
} // namespace zen
