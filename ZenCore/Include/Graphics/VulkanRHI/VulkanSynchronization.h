#pragma once
#include <queue>
#include "VulkanCommon.h"
#include "Graphics/RHI/RHIResource.h"

#define VK_WAIT_FENCE_TIME_NS (33 * 1000 * 1000LL)

namespace zen
{
class VulkanDevice;
class VulkanFenceManager;
class VulkanCommandBuffer;
class VulkanQueue;
struct VulkanTexture;

class VulkanFence
{
public:
    explicit VulkanFence(VulkanFenceManager* pOwner, bool createSignaled = false);

    ~VulkanFence() = default;

    VkFence GetVkHandle() const
    {
        return m_fence;
    }

    bool IsSignaled() const
    {
        return m_state == State::eSignaled;
    }

    VulkanFenceManager* GetOwner() const
    {
        return m_pOwner;
    }

private:
    enum class State
    {
        eInitial,
        eSignaled
    };
    VkFence m_fence{VK_NULL_HANDLE};
    VulkanFenceManager* m_pOwner{nullptr};
    State m_state{State::eInitial};

    friend class VulkanFenceManager;
};

class VulkanFenceManager
{
public:
    explicit VulkanFenceManager(VulkanDevice* pDevice) : m_pDevice(pDevice) {}

    void Destroy();

    VulkanDevice* GetDevice() const
    {
        return m_pDevice;
    }

    VulkanFence* CreateFence(bool createSignaled = false);

    void ReleaseFence(VulkanFence*& fence);

    bool IsFenceSignaled(VulkanFence* pFence);

    bool WaitForFence(VulkanFence* pFence, uint64_t timeNS);

    void ResetFence(VulkanFence* pFence);

    void WaitAndReleaseFence(VulkanFence*& fence, uint64_t timeNS);

private:
    void DestroyFence(VulkanFence* pFence);

    VulkanDevice* m_pDevice{nullptr};
    HeapVector<VulkanFence*> m_usedFences;
    std::queue<VulkanFence*> m_freeFences;
};

class VulkanSemaphore
{
public:
    explicit VulkanSemaphore(VulkanDevice* pDevice,
                             VkSemaphoreType semaphoreType = VK_SEMAPHORE_TYPE_BINARY,
                             uint64_t initialValue         = 0);

    virtual ~VulkanSemaphore();

    VkSemaphore GetVkHandle() const
    {
        return m_semaphore;
    }

    void SetDebugName(NameID name);

    bool IsTimeline() const
    {
        return m_type == VK_SEMAPHORE_TYPE_TIMELINE;
    }

    uint64_t GetCounterValue() const;

    bool Wait(uint64_t value, uint64_t timeNS) const;

    // CPU-side queue acceptance, not GPU completion or the semaphore's native state.
    // Read and publication are serialized with queue submission.
    uint64_t GetSignalGeneration() const
    {
        return m_signalGeneration;
    }

    uint64_t GetSignalSubmissionSerial(const VulkanQueue* pQueue, uint64_t previousGeneration) const
    {
        return m_pSignalQueue == pQueue && m_signalGeneration != previousGeneration ?
            m_signalSubmissionSerial :
            0;
    }

private:
    friend class VulkanQueue;
    friend class VulkanSemaphoreManager;

    VulkanDevice* m_pDevice{nullptr};
    VkSemaphore m_semaphore{VK_NULL_HANDLE};
    VkSemaphoreType m_type{VK_SEMAPHORE_TYPE_BINARY};
    const VulkanQueue* m_pSignalQueue{nullptr};
    uint64_t m_signalSubmissionSerial{0};
    uint64_t m_signalGeneration{0};
};

class VulkanSemaphoreManager
{
public:
    explicit VulkanSemaphoreManager(VulkanDevice* pDevice) : m_pDevice(pDevice) {}

    void Destroy();

    VulkanSemaphore* GetOrCreateSemaphore();

    void ReleaseSemaphore(VulkanSemaphore*& sem);

    // The caller must prove all semaphore operations have completed. Unlike recycling,
    // destruction also supports an acquired binary semaphore that was never waited on.
    void DestroySemaphore(VulkanSemaphore*& sem);

private:
    VulkanDevice* m_pDevice{nullptr};
    HeapVector<VulkanSemaphore*> m_usedSemaphores;
    std::queue<VulkanSemaphore*> m_freeSemaphores;
#if defined(ZEN_DEBUG)
    uint32_t m_allocatedSemaphoreCount{0};
#endif
};

class VulkanPipelineBarrier
{
public:
    // for Image-only transitions
    void AddImageBarrier(VkImage image,
                         VkImageLayout srcLayout,
                         VkImageLayout dstLayout,
                         const VkImageSubresourceRange& range);

    void AddImageBarrier(VkImage image,
                         VkImageLayout srcLayout,
                         VkImageLayout dstLayout,
                         const VkImageSubresourceRange& range,
                         VkAccessFlags srcAccess,
                         VkAccessFlags dstAccess);

    void AddBufferBarrier(VkBuffer buffer,
                          uint64_t offset,
                          uint64_t size,
                          VkAccessFlags srcAccess,
                          VkAccessFlags dstAccess);

    void AddMemoryBarrier(VkAccessFlags srcAccess, VkAccessFlags dstAccess);

    // for Image-only transitions
    void ExecuteImageBarriersOnly(VkCommandBuffer cmdBuffer);

    void Execute(VkCommandBuffer cmdBuffer,
                 VkPipelineStageFlags srcStageFlags,
                 VkPipelineStageFlags dstStageFlags);

private:
    static VkPipelineStageFlags VkLayoutToPipelineStageFlags(VkImageLayout layout);

    static VkAccessFlags VkLayoutToAccessFlags(VkImageLayout layout);

    VkPipelineStageFlags m_srcStageFlags{0};
    VkPipelineStageFlags m_dstStageFlags{0};

    HeapVector<VkImageMemoryBarrier> m_imageBarriers;
    HeapVector<VkMemoryBarrier> m_memoryBarriers;
    HeapVector<VkBufferMemoryBarrier> m_bufferBarriers;
};
} // namespace zen
