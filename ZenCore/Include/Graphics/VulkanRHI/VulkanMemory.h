#pragma once
#include <vk_mem_alloc.h>
#include <atomic>
#include "Templates/HashMap.h"
#include "Graphics/RHI/RHICommon.h"

namespace zen
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

    void Init(VkInstance instance, VkPhysicalDevice gpu, VkDevice device, bool bufferDeviceAddress);

    bool AllocImage(const VkImageCreateInfo* pImageCI,
                    bool cpuReadable,
                    VkImage* pImage,
                    VulkanMemoryAllocation* pAllocation,
                    uint32_t size);

    void FreeImage(VkImage image, const VulkanMemoryAllocation& memAlloc);

    void AllocBuffer(uint32_t size,
                     const VkBufferCreateInfo* pBufferCI,
                     RHIBufferAllocateType allocType,
                     VkBuffer* pBuffer,
                     VulkanMemoryAllocation* pAllocation);

    uint8_t* MapBuffer(const VulkanMemoryAllocation& memAlloc);

    void UnmapBuffer(const VulkanMemoryAllocation& memAlloc);

    void FreeBuffer(VkBuffer buffer, const VulkanMemoryAllocation& memAlloc);

private:
    static void VKAPI_PTR MemoryAllocated(VmaAllocator allocator,
                                          uint32_t memoryType,
                                          VkDeviceMemory memory,
                                          VkDeviceSize size,
                                          void* userData);
    static void VKAPI_PTR MemoryFreed(VmaAllocator allocator,
                                      uint32_t memoryType,
                                      VkDeviceMemory memory,
                                      VkDeviceSize size,
                                      void* userData);
    void TrackMemory(uint32_t memoryType, VkDeviceSize size, bool allocated);

    VmaPool GetOrCreateSmallAllocPools(MemoryTypeIndex memTypeIndex);

    VmaAllocator m_vmaAllocator{VK_NULL_HANDLE};
    HashMap<MemoryTypeIndex, VmaPool> m_smallPools;
    // VMA block commitments, including retained pools and resources awaiting retirement.
    // Driver-private and swapchain allocations are outside this allocator's scope.
    VkPhysicalDeviceMemoryProperties m_memoryProperties{};
    std::atomic<uint64_t> m_liveBytes{0};
    std::atomic<uint64_t> m_peakBytes{0};
    std::atomic<uint64_t> m_liveDeviceBytes{0};
    std::atomic<uint64_t> m_peakDeviceBytes{0};
    bool m_trackMemory{false};
};
} // namespace zen
