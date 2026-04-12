#pragma once
#include "Templates/HeapVector.h"
#include "VulkanHeaders.h"

namespace zen
{
class VulkanQueue;
class VulkanDevice;
class VulkanCommandBufferManager;
class VulkanCommandBuffer;
class VulkanFence;
class VulkanSemaphore;


enum class VulkanCommandBufferType
{
    ePrimary   = 0,
    eSecondary = 1
};

struct VulkanCommandPool
{
    VkCommandPool vkHandle{VK_NULL_HANDLE};
    uint32_t queueFamilyIndex{};
};

class VulkanCommandBufferPool
{
public:
    VulkanCommandBufferPool(VulkanDevice* pDevice, VulkanCommandBufferManager* pMgr) :
        m_pDevice(pDevice), m_pOwner(pMgr)
    {}

    ~VulkanCommandBufferPool();

    void Init(uint32_t queueFamilyIndex);

    VulkanDevice* GetDevice() const
    {
        return m_pDevice;
    }

    VkCommandPool GetVkCmdPool() const
    {
        return m_cmdPool;
    }

    VulkanCommandBufferManager* GetManager() const
    {
        return m_pOwner;
    }

    void RefreshFenceStatus(VulkanCommandBuffer* pSkipCmdBuffer);

private:
    VulkanCommandBuffer* CreateCmdBuffer(bool isUploadOnly);
    void FreeUnusedCmdBuffers(VulkanQueue* pQueue);

    VulkanDevice* m_pDevice{nullptr};
    VulkanCommandBufferManager* m_pOwner;
    VkCommandPool m_cmdPool{VK_NULL_HANDLE};
    HeapVector<VulkanCommandBuffer*> m_usedCmdBuffers;
    HeapVector<VulkanCommandBuffer*> m_freeCmdBuffers;

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

    ~VulkanCommandBuffer();

    void AddWaitSemaphore(VkPipelineStageFlags waitFlags, VulkanSemaphore* pSem);

    void AddWaitSemaphore(VkPipelineStageFlags waitFlags, const HeapVector<VulkanSemaphore*>& sems);

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
        return m_pCmdBufferPool;
    }

protected:
    VulkanCommandBuffer(VulkanCommandBufferPool* pPool, bool isUploadOnly);

private:
    void AllocMemory();

    void FreeMemory();

    void RefreshFenceStatus();

    void MarkSemaphoresAsSubmitted()
    {
        m_waitFlags.clear();
        m_submittedWaitSemaphores = std::move(m_waitSemaphores);
        m_waitSemaphores.clear();
    }

    VulkanCommandBufferPool* m_pCmdBufferPool{nullptr};
    bool m_isUploadOnly{false};
    VkCommandBuffer m_cmdBuffer{VK_NULL_HANDLE};
    State m_state{State::eNotAllocated};

    // Sync objects
    HeapVector<VkPipelineStageFlags> m_waitFlags;
    // DO NOT own the semaphores, only hold reference
    HeapVector<VulkanSemaphore*> m_waitSemaphores;
    HeapVector<VulkanSemaphore*> m_submittedWaitSemaphores;
    VulkanFence* m_pFence;
    uint64_t m_fenceSignaledCounter{0};

    friend class VulkanCommandBufferPool;
    friend class VulkanCommandBufferManager;
    friend class VulkanQueue;
};

class VulkanCommandBufferManager
{
public:
    VulkanCommandBufferManager(VulkanDevice* pDevice, VulkanQueue* pQueue);
    ~VulkanCommandBufferManager() = default;

    VulkanCommandBuffer* GetActiveCommandBuffer();

    VulkanCommandBuffer* GetActiveCommandBufferDirect() const
    {
        return m_pActiveCmdBuffer;
    }

    VulkanCommandBuffer* GetUploadCommandBuffer();

    void SubmitActiveCmdBuffer(const HeapVector<VulkanSemaphore*>& signalSemaphores);

    void SubmitActiveCmdBuffer(VulkanSemaphore* pSignalSemaphore);

    void SubmitActiveCmdBuffer();

    void SubmitActiveCmdBufferForPresent(VulkanSemaphore* pSignalSemaphore);

    void SubmitUploadCmdBuffer();

    void SetupNewActiveCmdBuffer();

    void FreeUnusedCmdBuffers();

    bool HasPendingActiveCmdBuffer()
    {
        return m_pActiveCmdBuffer != nullptr;
    }

    bool HasPendingUploadCmdBuffer()
    {
        return m_pUploadCmdBuffer != nullptr;
    }

    void WaitForCmdBuffer(VulkanCommandBuffer* pCmdBuffer, float timeInSecondsToWait = 1.0f);

    // Update the fences of all cmd buffers except SkipCmdBuffer
    void RefreshFenceStatus(VulkanCommandBuffer* pSkipCmdBuffer = nullptr)
    {
        m_pool.RefreshFenceStatus(pSkipCmdBuffer);
    }

private:
    VulkanDevice* m_pDevice{nullptr};
    VulkanQueue* m_pQueue{nullptr};
    VulkanCommandBufferPool m_pool;
    // use dedicated command buffer for upload workloads
    VulkanCommandBuffer* m_pActiveCmdBuffer{nullptr};
    VulkanCommandBuffer* m_pUploadCmdBuffer{nullptr};

    // semaphores for cmd buffer, configured through RHIOptions
    VulkanSemaphore* m_pActiveCmdBufferSemaphore{nullptr};
    VulkanSemaphore* m_pUploadCmdBufferSemaphore{nullptr};
    // upload cmd buffer semaphores waited by active cmd buffer
    HeapVector<VulkanSemaphore*> m_uploadCompleteSemaphores;
    // upload cmd buffer semaphores waited by upload cmd buffer
    HeapVector<VulkanSemaphore*> m_renderCompleteSemaphores;
};
} // namespace zen
