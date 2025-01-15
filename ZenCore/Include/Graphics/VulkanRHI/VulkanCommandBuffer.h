#pragma once

#include "Common/HashMap.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"

namespace zen::rhi
{
class VulkanQueue;
class VulkanDevice;
class VulkanCommandBufferManager;
class VulkanCommandBuffer;
class VulkanFence;
class VulkanSemaphore;

struct VulkanCommandPool
{
    VkCommandPool vkHandle{VK_NULL_HANDLE};
    uint32_t queueFamilyIndex{};
};

class VulkanCommandBufferPool
{
public:
    VulkanCommandBufferPool(VulkanDevice* device, VulkanCommandBufferManager* mgr) :
        m_device(device), m_owner(mgr)
    {}

    ~VulkanCommandBufferPool();

    void Init(uint32_t queueFamilyIndex);

    VulkanDevice* GetDevice() const
    {
        return m_device;
    }

    VkCommandPool GetVkCmdPool() const
    {
        return m_cmdPool;
    }

    VulkanCommandBufferManager* GetManager() const
    {
        return m_owner;
    }

    void RefreshFenceStatus(VulkanCommandBuffer* skipCmdBuffer);

private:
    VulkanCommandBuffer* CreateCmdBuffer(bool isUploadOnly);
    void FreeUnusedCmdBuffers(VulkanQueue* queue);

    VulkanDevice* m_device{nullptr};
    VulkanCommandBufferManager* m_owner;
    VkCommandPool m_cmdPool{VK_NULL_HANDLE};
    std::vector<VulkanCommandBuffer*> m_usedCmdBuffers;
    std::vector<VulkanCommandBuffer*> m_freeCmdBuffers;

    friend class VulkanCommandBufferManager;
};

class VulkanCommandBuffer
{
public:
    enum class State : uint32_t
    {
        eReadyForBegin,
        eHasBegun,
        eInsideRenderPass,
        eHasEnded,
        eSubmitted,
        eNotAllocated,
        eNeedReset
    };

    void AddWaitSemaphore(VkPipelineStageFlags waitFlags, VulkanSemaphore* sem);

    void AddWaitSemaphore(VkPipelineStageFlags waitFlags,
                          const std::vector<VulkanSemaphore*>& sems);

    VkCommandBuffer GetVkHandle() const
    {
        return m_cmdBuffer;
    }

    void Begin();

    void End();

    bool IsSubmitted() const
    {
        return m_state == State::eSubmitted;
    }

    bool HasBegun() const
    {
        return m_state == State::eHasBegun || m_state == State::eInsideRenderPass;
    }

    bool IsOutsideRenderPass() const
    {
        return m_state == State::eHasBegun;
    }

    void EndRenderPass();

    auto GetFenceSignaledCounter() const
    {
        return m_fenceSignaledCounter;
    }

    VulkanCommandBufferPool* GetOwner() const
    {
        return m_cmdBufferPool;
    }

protected:
    VulkanCommandBuffer(VulkanCommandBufferPool* pool, bool isUploadOnly);
    ~VulkanCommandBuffer();

private:
    void AllocMemory();

    void FreeMemory();

    void RefreshFenceStatus();

    void MarkSemaphoresAsSubmitted()
    {
        m_waitFlags.clear();
        m_submittedWaitSemaphores = m_waitSemaphores;
        m_waitSemaphores.clear();
    }

    VulkanCommandBufferPool* m_cmdBufferPool{nullptr};
    bool m_isUploadOnly{false};
    VkCommandBuffer m_cmdBuffer{VK_NULL_HANDLE};
    State m_state{State::eNotAllocated};

    // Sync objects
    std::vector<VkPipelineStageFlags> m_waitFlags;
    // DO NOT own the semaphores, only hold reference
    std::vector<VulkanSemaphore*> m_waitSemaphores;
    std::vector<VulkanSemaphore*> m_submittedWaitSemaphores;
    VulkanFence* m_fence;
    uint64_t m_fenceSignaledCounter{0};

    friend class VulkanCommandBufferPool;
    friend class VulkanCommandBufferManager;
    friend class VulkanQueue;
};

class VulkanCommandBufferManager
{
public:
    VulkanCommandBufferManager(VulkanDevice* device, VulkanQueue* queue);
    ~VulkanCommandBufferManager() = default;

    VulkanCommandBuffer* GetActiveCommandBuffer();

    VulkanCommandBuffer* GetActiveCommandBufferDirect() const
    {
        return m_activeCmdBuffer;
    }

    VulkanCommandBuffer* GetUploadCommandBuffer();

    void SubmitActiveCmdBuffer(const std::vector<VulkanSemaphore*>& signalSemaphores);

    void SubmitActiveCmdBuffer(VulkanSemaphore* signalSemaphore);

    void SubmitActiveCmdBuffer();

    void SubmitActiveCmdBufferForPresent(VulkanSemaphore* signalSemaphore);

    void SubmitUploadCmdBuffer();

    void SetupNewActiveCmdBuffer();

    void FreeUnusedCmdBuffers();

    bool HasPendingActiveCmdBuffer()
    {
        return m_activeCmdBuffer != nullptr;
    }

    bool HasPendingUploadCmdBuffer()
    {
        return m_uploadCmdBuffer != nullptr;
    }

    void WaitForCmdBuffer(VulkanCommandBuffer* cmdBuffer, float timeInSecondsToWait = 1.0f);

    // Update the fences of all cmd buffers except SkipCmdBuffer
    void RefreshFenceStatus(VulkanCommandBuffer* skipCmdBuffer = nullptr)
    {
        m_pool.RefreshFenceStatus(skipCmdBuffer);
    }

private:
    VulkanDevice* m_device{nullptr};
    VulkanQueue* m_queue{nullptr};
    VulkanCommandBufferPool m_pool;
    // use dedicated command buffer for upload workloads
    VulkanCommandBuffer* m_activeCmdBuffer{nullptr};
    VulkanCommandBuffer* m_uploadCmdBuffer{nullptr};

    // semaphores for cmd buffer, configured through RHIOptions
    VulkanSemaphore* m_activeCmdBufferSemaphore{nullptr};
    VulkanSemaphore* m_uploadCmdBufferSemaphore{nullptr};
    // upload cmd buffer semaphores waited by active cmd buffer
    std::vector<VulkanSemaphore*> m_uploadCompleteSemaphores;
    // upload cmd buffer semaphores waited by upload cmd buffer
    std::vector<VulkanSemaphore*> m_renderCompleteSemaphores;
};
} // namespace zen::rhi
