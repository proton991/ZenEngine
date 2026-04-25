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

    uint64_t SubmitPendingWorkloads();

    void ProcessPendingWorkloads(uint64_t timeToWaitNS);

    void WaitForSubmission(uint64_t submissionSerial, uint64_t timeToWaitNS);

private:
    struct WorkloadMergeResult
    {
        HeapVector<VulkanWorkload*> workloadsToSubmit;
        size_t totalWaitSemaphoreCount{0};
        size_t totalSignalSemaphoreCount{0};
        size_t totalCommandBufferCount{0};
    };

    struct TimelineSubmitBatch
    {
        HeapVector<VkSubmitInfo> submitInfos;
        HeapVector<VkTimelineSemaphoreSubmitInfo> timelineSubmitInfos;
        HeapVector<VkCommandBuffer> commandBuffers;
        HeapVector<VkSemaphore> waitSemaphores;
        HeapVector<VkSemaphore> signalSemaphores;
        HeapVector<VkPipelineStageFlags> waitStageMasks;
        HeapVector<uint64_t> waitSemaphoreValues;
        HeapVector<uint64_t> signalSemaphoreValues;
    };

    static bool CanMergeWorkloads(const VulkanWorkload* pPreviousWorkload,
                                  const VulkanWorkload* pCurrentWorkload);

    void UpdateLastSubmittedCmdBuffer(VulkanCommandBuffer* pCmdBuffer);

    uint64_t SubmitWorkloadsWithFences();

    uint64_t SubmitWorkloadsWithTimelineSemaphore();

    void MergeWorkloads(const HeapVector<VulkanWorkload*>& workloadsToSubmit,
                        WorkloadMergeResult& outMergeResult);

    void BuildTimelineSubmitBatch(const WorkloadMergeResult& mergeResult,
                                  TimelineSubmitBatch& outSubmitBatch);

    void AppendTimelineSubmitWorkload(VulkanWorkload* pWorkload,
                                      TimelineSubmitBatch& outSubmitBatch);

    void QueueSubmittedWorkloads(const HeapVector<VulkanWorkload*>& workloadsToSubmit);

    VulkanWorkload* AcquireWorkload();

    void ReleaseWorkload(VulkanWorkload* pWorkload);

    void DestroyWorkload(VulkanWorkload* pWorkload);

    VulkanDevice* m_pDevice{nullptr};
    VkQueue m_handle{VK_NULL_HANDLE};
    uint32_t m_familyIndex;
    uint32_t m_queueIndex;

    VulkanCommandBuffer* m_pLastSubmittedCmdBuffer{nullptr};

    HeapVector<FVulkanCommandBufferPool*> m_cmdBufferPools;
    HeapVector<VulkanWorkload*> m_workloadPool;

    Queue<VulkanWorkload*> m_workloadsPendingSubmit;  // queued workloads, need to submit
    Queue<VulkanWorkload*> m_workloadsPendingProcess; // submitted workloads, need to wait
    uint64_t m_nextSubmissionSerial{0};
    uint64_t m_lastCompletedSubmissionSerial{0};
    VulkanSemaphore* m_pTimelineSemaphore{nullptr};

    friend class VulkanRHI;
    friend class VulkanCommandContextBase;
};
} // namespace zen
