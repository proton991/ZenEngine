#pragma once
#include <vk_mem_alloc.h>
#include <atomic>
#include "Graphics/RHI/RHICommon.h"
#include "Graphics/RHI/RHIGPUMemoryStats.h"

namespace zen
{
struct VulkanMemoryAllocation
{
    VmaAllocation handle{VK_NULL_HANDLE};
    VmaAllocationInfo info{};
};

class VulkanMemoryAllocator
{
public:
    VulkanMemoryAllocator() = default;

    ~VulkanMemoryAllocator();

    void Init(VkInstance instance,
              VkPhysicalDevice gpu,
              VkDevice device,
              bool bufferDeviceAddress,
              bool memoryBudget = false);

    void BeginFrame(uint32_t frame);

    RHIGPUMemoryStats GetGPUMemoryStats() const;

    bool AllocImage(const VkImageCreateInfo* pImageCI,
                    bool cpuReadable,
                    VkImage* pImage,
                    VulkanMemoryAllocation* pAllocation);

    void FreeImage(VkImage image, const VulkanMemoryAllocation& memAlloc);

    bool AllocBuffer(uint64_t size,
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

    VmaAllocator m_vmaAllocator{VK_NULL_HANDLE};
    // VMA block commitments, including retained pools and resources awaiting retirement.
    // Driver-private and swapchain allocations are outside this allocator's scope.
    VkPhysicalDeviceMemoryProperties m_memoryProperties{};
    std::atomic<uint64_t> m_liveBytes{0};
    std::atomic<uint64_t> m_peakBytes{0};
    std::atomic<uint64_t> m_liveDeviceBytes{0};
    std::atomic<uint64_t> m_peakDeviceBytes{0};
    bool m_logMemoryStats{false};
    bool m_memoryBudget{false};
};
} // namespace zen
