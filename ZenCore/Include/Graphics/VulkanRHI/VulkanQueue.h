#pragma once
#include <atomic>
#include "Graphics/RHI/RHICommon.h"
#include "VulkanHeaders.h"
#include "Templates/HeapVector.h"
#include "Templates/Queue.h"

namespace zen
{
class VulkanWorkload;
class VulkanDevice;
class VulkanSemaphore;
enum class VulkanCommandBufferType;
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

    void EnqueueWorkload(VulkanWorkload* pWorkload)
    {
        m_workloadsPendingSubmit.Push(pWorkload);
    }

    // serial is the last successfully submitted serial from this call, or zero.
    RHISubmissionResult SubmitPendingWorkloads(uint64_t& serial);

    void DiscardPendingWorkloads(bool uncertain = false);

    void ProcessPendingWorkloads(uint64_t timeToWaitNS, uint64_t maxSubmissionSerial = UINT64_MAX);

    bool WaitForSubmission(uint64_t submissionSerial, uint64_t timeToWaitNS);

    uint64_t GetLastSubmittedSerial() const
    {
        return m_lastSubmittedSerial;
    }

    uint64_t GetLastCompletedSerial() const
    {
        return m_lastCompletedSerial;
    }

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

    RHISubmissionResult SubmitWorkloadsWithFences(uint64_t& serial);

    RHISubmissionResult SubmitWorkloadsWithTimelineSemaphore(uint64_t& serial);

    void MergeWorkloads(const HeapVector<VulkanWorkload*>& workloadsToSubmit,
                        WorkloadMergeResult& outMergeResult);

    void BuildTimelineSubmitBatch(const WorkloadMergeResult& mergeResult,
                                  TimelineSubmitBatch& outSubmitBatch);

    void AppendTimelineSubmitWorkload(VulkanWorkload* pWorkload,
                                      TimelineSubmitBatch& outSubmitBatch);

    void QueueSubmittedWorkload(VulkanWorkload* pWorkload, uint64_t submissionSerial);

    VulkanWorkload* AcquireWorkload();

    void ReleaseWorkload(VulkanWorkload* pWorkload);

    void DestroyWorkload(VulkanWorkload* pWorkload);

    VulkanDevice* m_pDevice{nullptr};
    VkQueue m_handle{VK_NULL_HANDLE};
    uint32_t m_familyIndex;
    uint32_t m_queueIndex;

    HeapVector<FVulkanCommandBufferPool*> m_cmdBufferPools;
    HeapVector<VulkanWorkload*> m_workloadPool;
    HeapVector<VulkanWorkload*>
        m_abandonedWorkloads; // Uncertain submission; keep until device teardown.

    Queue<VulkanWorkload*> m_workloadsPendingSubmit;  // queued workloads, need to submit
    Queue<VulkanWorkload*> m_workloadsPendingProcess; // submitted workloads, need to wait
    uint64_t m_lastSubmittedSerial{0};
    // Lifetime queries can read completion while the submission thread polls the queue.
    std::atomic<uint64_t> m_lastCompletedSerial{0};
    VulkanSemaphore* m_pTimelineSemaphore{nullptr};

    friend class VulkanRHI;
    friend class VulkanCommandContextBase;
};
} // namespace zen
