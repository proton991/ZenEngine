#pragma once
#include <vk_mem_alloc.h>
#include "Common/HashMap.h"

namespace zen::rhi
{
struct VulkanMemoryAllocation
{
    VmaAllocation handle{VK_NULL_HANDLE};
    VmaAllocationInfo info{};
};

using MemoryTypeIndex = uint32_t;

class VulkanMemoryAllocator
{
public:
    VulkanMemoryAllocator() = default;

    void Init(VkInstance instance, VkPhysicalDevice gpu, VkDevice device);

    void CreateImage(const VkImageCreateInfo* imageCI,
                     bool cpuReadable,
                     VkImage* image,
                     VulkanMemoryAllocation* allocation,
                     uint32_t size);

    void DestroyImage(VkImage image, const VulkanMemoryAllocation& memAlloc);

private:
    VmaPool GetOrCreateSmallAllocPools(MemoryTypeIndex memTypeIndex);

    VmaAllocator m_vmaAllocator{VK_NULL_HANDLE};
    HashMap<MemoryTypeIndex, VmaPool> m_smallPools;
};
} // namespace zen::rhi