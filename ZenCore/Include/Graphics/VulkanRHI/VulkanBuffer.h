#pragma once

#include "VulkanHeaders.h"
#include "VulkanMemory.h"

namespace zen::rhi
{
struct VulkanBuffer
{
    VkBuffer buffer{VK_NULL_HANDLE};
    uint32_t allocatedSize{0};
    uint32_t requiredSize{0};
    VkBufferView bufferView{VK_NULL_HANDLE};
    VulkanMemoryAllocation memAlloc{};
};
} // namespace zen::rhi