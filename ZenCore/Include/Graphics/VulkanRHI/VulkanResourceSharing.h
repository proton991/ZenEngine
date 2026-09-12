#pragma once
#include "VulkanHeaders.h"

namespace zen
{
// The allocator consumes create-info synchronously while its queue-family storage is alive.
// Clear the borrowed pointer before returning so cached create-info cannot retain stack storage.
template <typename CreateInfo, typename Allocate>
void AllocateWithQueueSharing(CreateInfo& info,
                              uint32_t graphicsFamily,
                              uint32_t transferFamily,
                              bool transferUsage,
                              Allocate&& allocate)
{
    const uint32_t families[] = {graphicsFamily, transferFamily};
    const bool concurrent     = transferUsage && graphicsFamily != transferFamily;
    info.sharingMode          = concurrent ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
    info.queueFamilyIndexCount = concurrent ? 2 : 0;
    info.pQueueFamilyIndices   = concurrent ? families : nullptr;
    allocate();
    info.queueFamilyIndexCount = 0;
    info.pQueueFamilyIndices   = nullptr;
}
} // namespace zen
