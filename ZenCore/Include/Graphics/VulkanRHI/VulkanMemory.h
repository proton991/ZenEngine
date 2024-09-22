#pragma once
#include <vk_mem_alloc.h>
#include "Common/HashMap.h"
#include "Graphics/RHI/RHICommon.h"

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

    ~VulkanMemoryAllocator();

    void Init(VkInstance instance, VkPhysicalDevice gpu, VkDevice device);

    void AllocImage(const VkImageCreateInfo* imageCI,
                    bool cpuReadable,
                    VkImage* image,
                    VulkanMemoryAllocation* allocation,
                    uint32_t size);

    void FreeImage(VkImage image, const VulkanMemoryAllocation& memAlloc);

    void AllocBuffer(uint32_t size,
                     const VkBufferCreateInfo* bufferCI,
                     BufferAllocateType allocType,
                     VkBuffer* buffer,
                     VulkanMemoryAllocation* allocation);

    uint8_t* MapBuffer(const VulkanMemoryAllocation& memAlloc);

    void UnmapBuffer(const VulkanMemoryAllocation& memAlloc);

    void FreeBuffer(VkBuffer buffer, const VulkanMemoryAllocation& memAlloc);

private:
    VmaPool GetOrCreateSmallAllocPools(MemoryTypeIndex memTypeIndex);

    VmaAllocator m_vmaAllocator{VK_NULL_HANDLE};
    HashMap<MemoryTypeIndex, VmaPool> m_smallPools;
};
} // namespace zen::rhi