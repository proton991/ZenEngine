#pragma once
#include "VulkanCommandBuffer.h"
#include "VulkanHeaders.h"
#include "Templates/HeapVector.h"
#include "Templates/Queue.h"

namespace zen
{
class VulkanWorkload;
class VulkanDevice;
class VulkanCommandBuffer;
class FVulkanCommandBufferPool;

class VulkanQueue
{
public:
    VulkanQueue(VulkanDevice* pDevice, uint32_t familyIndex);

    ~VulkanQueue();

    FVulkanCommandBufferPool* AcquireCommandBufferPool(VulkanCommandBufferType type);

    void RecycleCommandBufferPool(FVulkanCommandBufferPool* pCmdBufferPool);

    uint32_t GetFamilyIndex() const
    {
        return m_familyIndex;
    }

    uint32_t GetQueueIndex() const
    {
        return m_queueIndex;
    }

    VkQueue GetVkHandle() const
    {
        return m_handle;
    }

    VulkanCommandBuffer* GetLastSubmittedCmdBuffer() const
    {
        return m_pLastSubmittedCmdBuffer;
    }

    void GetLastSubmitInfo(VulkanCommandBuffer*& cmdBuffer, uint64_t* pFenceSignaledCounter) const;

    void Submit(VulkanCommandBuffer* pCmdBuffer,
                uint32_t numSignalSemaphores,
                VkSemaphore* pSignalSemaphores);

    void Submit(VulkanCommandBuffer* pCmdBuffer, VkSemaphore signalSemaphore);

    void Submit(VulkanCommandBuffer* pCmdBuffer);

    void SubmitWorkloads();

    void ProcessPendingWorkloads(uint64_t timeToWaitNS);

    void WaitForSubmission(uint64_t submissionSerial, uint64_t timeToWaitNS);

private:
    void UpdateLastSubmittedCmdBuffer(VulkanCommandBuffer* pCmdBuffer);

    VulkanDevice* m_pDevice{nullptr};
    VkQueue m_handle{VK_NULL_HANDLE};
    uint32_t m_familyIndex;
    uint32_t m_queueIndex;

    VulkanCommandBuffer* m_pLastSubmittedCmdBuffer{nullptr};

    HeapVector<FVulkanCommandBufferPool*> m_cmdBufferPools;

    Queue<VulkanWorkload*> m_workloadsPendingSubmit;  // queued workloads, need to submit
    Queue<VulkanWorkload*> m_workloadsPendingProcess; // submitted workloads, need to wait
    uint64_t m_nextSubmissionSerial{0};
    uint64_t m_lastCompletedSubmissionSerial{0};
    VulkanSemaphore* m_pTimelineSemaphore{nullptr};

    friend class VulkanRHI;
    friend class VulkanCommandContextBase;
};
} // namespace zen
