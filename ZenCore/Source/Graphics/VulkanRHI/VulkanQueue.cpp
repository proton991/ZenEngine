#include <algorithm>
#include "Graphics/VulkanRHI/VulkanQueue.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanSynchronization.h"
#include "Graphics/VulkanRHI/VulkanCommandList.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanBuffer.h"
#include <chrono>

namespace zen
{
namespace
{
void AppendCommandBufferPool(HeapVector<FVulkanCommandBufferPool*>& pools,
                             FVulkanCommandBufferPool* pPool)
{
    if (pPool != nullptr && std::find(pools.begin(), pools.end(), pPool) == pools.end())
    {
        pools.push_back(pPool);
    }
}
} // namespace

VulkanQueue::VulkanQueue(VulkanDevice* pDevice, uint32_t familyIndex, uint32_t queueIndex) :
    m_pDevice(pDevice), m_familyIndex(familyIndex), m_queueIndex(queueIndex)
{
    vkGetDeviceQueue(m_pDevice->GetVkHandle(), m_familyIndex, m_queueIndex, &m_handle);

    if (m_pDevice->SupportsTimelineSemaphore())
    {
        m_pTimelineSemaphore = ZEN_NEW() VulkanSemaphore(m_pDevice, VK_SEMAPHORE_TYPE_TIMELINE, 0);
    }
}

VulkanQueue::~VulkanQueue()
{
    while (!m_workloadsPendingSubmit.Empty())
    {
        VulkanWorkload* pWorkload = m_workloadsPendingSubmit.Peek();
        m_workloadsPendingSubmit.Pop();
        DestroyWorkload(pWorkload);
    }

    while (!m_workloadsPendingProcess.Empty())
    {
        VulkanWorkload* pWorkload = m_workloadsPendingProcess.Peek();
        m_workloadsPendingProcess.Pop();
        DestroyWorkload(pWorkload);
    }

    for (VulkanWorkload* pWorkload : m_workloadPool)
    {
        DestroyWorkload(pWorkload);
    }

    for (VulkanWorkload* workload : m_abandonedWorkloads)
    {
        DestroyWorkload(workload);
    }

    GVulkanRHI->GetLifetimeTracker().RemoveQueue(this);

    for (FVulkanCommandBufferPool* pCmdBufferPool : m_cmdBufferPools)
    {
        ZEN_DELETE(pCmdBufferPool);
    }

    if (m_pTimelineSemaphore != nullptr)
    {
        ZEN_DELETE(m_pTimelineSemaphore);
        m_pTimelineSemaphore = nullptr;
    }
}

FVulkanCommandBufferPool* VulkanQueue::AcquireCommandBufferPool(VulkanCommandBufferType type)
{
    FVulkanCommandBufferPool* pResult                  = nullptr;
    HeapVector<FVulkanCommandBufferPool*>::iterator it = m_cmdBufferPools.end();

    while (it != m_cmdBufferPools.begin())
    {
        --it;
        FVulkanCommandBufferPool* pCmdBufferPool = *it;

        if (pCmdBufferPool->GetCommandBufferType() == type)
        {
            m_cmdBufferPools.erase(it);
            pResult = pCmdBufferPool;
            break;
        }
    }

    if (pResult == nullptr)
    {
        pResult = ZEN_NEW() FVulkanCommandBufferPool(this, type);
    }

    return pResult;
}

void VulkanQueue::RecycleCommandBufferPool(FVulkanCommandBufferPool* pCmdBufferPool)
{
    VERIFY_EXPR(pCmdBufferPool->GetQueue() == this);
    pCmdBufferPool->FreeUnusedCommandBuffers();
    m_cmdBufferPools.emplace_back(pCmdBufferPool);
}

VulkanWorkload* VulkanQueue::AcquireWorkload()
{
    VulkanWorkload* result{};

    if (!m_workloadPool.empty())
    {
        VulkanWorkload* pWorkload = m_workloadPool.back();
        m_workloadPool.pop_back();
        result = pWorkload;
    }
    else
    {
        result = ZEN_NEW() VulkanWorkload(this);
    }

    return result;
}

void VulkanQueue::DiscardWorkload(VulkanWorkload* pWorkload)
{
    for (FVulkanCommandBuffer* buffer : pWorkload->m_commandBuffers)
    {
        buffer->Discard();
    }
    ReleaseWorkload(pWorkload);
}

void VulkanQueue::ReleaseWorkload(VulkanWorkload* pWorkload)
{
    if (pWorkload == nullptr)
    {
        return;
    }

    VERIFY_EXPR(pWorkload->m_pQueue == this);

    if (pWorkload->m_pFence != nullptr)
    {
        pWorkload->m_pFence->GetOwner()->ReleaseFence(pWorkload->m_pFence);
        pWorkload->m_pFence = nullptr;
    }

    pWorkload->m_commandBuffers.clear();
    GVulkanRHI->GetLifetimeTracker().ReleaseRecordings(pWorkload->m_lifetimeIds);
    pWorkload->m_lifetimeIds.clear();
    pWorkload->m_submissionSerial = 0;
    pWorkload->m_pMergedInto      = nullptr;
    pWorkload->m_waitSemaphoreInfos.clear();
    pWorkload->m_signalSemaphoreInfos.clear();

    for (VulkanWorkload* pWorkload : pWorkload->m_mergedWorkloads)
    {
        ReleaseWorkload(pWorkload);
    }

    pWorkload->m_mergedWorkloads.clear();

    GVulkanRHI->GetLifetimeTracker().Collect();
    m_workloadPool.push_back(pWorkload);
}

void VulkanQueue::DestroyWorkload(VulkanWorkload* pWorkload)
{
    if (pWorkload == nullptr)
    {
        return;
    }

    GVulkanRHI->GetLifetimeTracker().ReleaseRecordings(pWorkload->m_lifetimeIds);
    pWorkload->m_lifetimeIds.clear();

    if (pWorkload->m_pFence != nullptr)
    {
        pWorkload->m_pFence->GetOwner()->ReleaseFence(pWorkload->m_pFence);
        pWorkload->m_pFence = nullptr;
    }

    for (VulkanWorkload* pWorkload : pWorkload->m_mergedWorkloads)
    {
        DestroyWorkload(pWorkload);
    }

    pWorkload->m_mergedWorkloads.clear();

    ZEN_DELETE(pWorkload);
}

bool VulkanQueue::CanMergeWorkloads(const VulkanWorkload* pPreviousWorkload,
                                    const VulkanWorkload* pCurrentWorkload)
{
    // UE5's minimal-submit path only merges adjacent payloads when there is no queue-visible
    // synchronization or action required between them. In ZenEngine's current non-parallel
    // model, that reduces to "next has no waits" and "previous has no signals".
    return pPreviousWorkload->m_signalSemaphoreInfos.empty() &&
        pCurrentWorkload->m_waitSemaphoreInfos.empty();
}

static RHISubmissionResult SubmissionFailure(VkResult result, bool submittedPrefix)
{
    LOGE("Vulkan queue submission failed: {} (submitted prefix: {})", int32_t(result),
         submittedPrefix);
    // Vulkan guarantees unchanged resource/semaphore state for these errors only.
    const bool rejected =
        result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY;

    return rejected && !submittedPrefix ? RHISubmissionResult::eRejected :
                                          RHISubmissionResult::eFatal;
}

void VulkanQueue::DiscardPendingWorkloads(bool uncertain)
{
    while (!m_workloadsPendingSubmit.Empty())
    {
        VulkanWorkload* workload = m_workloadsPendingSubmit.Peek();
        m_workloadsPendingSubmit.Pop();

        if (uncertain)
        {
            m_abandonedWorkloads.push_back(workload);
        }
        else
        {
            DiscardWorkload(workload);
        }
    }
}

RHISubmissionResult VulkanQueue::SubmitWorkloadsWithFences(uint64_t& lastSubmissionSerial)
{
    RHISubmissionResult submissionResult = RHISubmissionResult::eSuccess;

    VulkanFenceManager* pFenceManager = m_pDevice->GetFenceManager();

    while (!m_workloadsPendingSubmit.Empty())
    {
        VulkanWorkload* pWorkload = m_workloadsPendingSubmit.Peek();
        // A rejected attempt keeps its unsignaled fence. Preserve it on retry.
        if (pWorkload->m_pFence == nullptr)
        {
            pWorkload->m_pFence = pFenceManager->CreateFence();
        }

        VkSubmitInfo submitInfo;
        InitVkStruct(submitInfo, VK_STRUCTURE_TYPE_SUBMIT_INFO);
        HeapVector<VkSemaphore> waitSemaphores;
        HeapVector<VkSemaphore> signalSemaphores;
        HeapVector<VkPipelineStageFlags> waitStageMasks;
        waitSemaphores.reserve(pWorkload->m_waitSemaphoreInfos.size());
        signalSemaphores.reserve(pWorkload->m_signalSemaphoreInfos.size());
        waitStageMasks.reserve(pWorkload->m_waitSemaphoreInfos.size());

        for (VulkanWorkload::WaitSemaphoreInfo const& waitInfo : pWorkload->m_waitSemaphoreInfos)
        {
            waitSemaphores.push_back(waitInfo.pSemaphore->GetVkHandle());
            waitStageMasks.push_back(waitInfo.waitFlags);
        }

        for (VulkanWorkload::SignalSemaphoreInfo const& signalInfo :
             pWorkload->m_signalSemaphoreInfos)
        {
            signalSemaphores.push_back(signalInfo.pSemaphore->GetVkHandle());
        }

        HeapVector<VkCommandBuffer> cmdBuffers;
        cmdBuffers.reserve(pWorkload->m_commandBuffers.size());

        for (FVulkanCommandBuffer* pCmdBuffer : pWorkload->m_commandBuffers)
        {
            cmdBuffers.push_back(pCmdBuffer->GetVkHandle());
        }

        submitInfo.commandBufferCount   = static_cast<uint32_t>(cmdBuffers.size());
        submitInfo.pCommandBuffers      = cmdBuffers.empty() ? nullptr : cmdBuffers.data();
        submitInfo.signalSemaphoreCount = static_cast<uint32_t>(signalSemaphores.size());
        submitInfo.pSignalSemaphores = signalSemaphores.empty() ? nullptr : signalSemaphores.data();
        submitInfo.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size());
        submitInfo.pWaitSemaphores    = waitSemaphores.empty() ? nullptr : waitSemaphores.data();
        submitInfo.pWaitDstStageMask  = waitStageMasks.empty() ? nullptr : waitStageMasks.data();

        VkFence submitFence =
            pWorkload->m_pFence != nullptr ? pWorkload->m_pFence->GetVkHandle() : VK_NULL_HANDLE;

        const VkResult result = vkQueueSubmit(m_handle, 1, &submitInfo, submitFence);

        if (result != VK_SUCCESS)
        {
            submissionResult = SubmissionFailure(result, lastSubmissionSerial != 0);
            break;
        }

        m_workloadsPendingSubmit.Pop();
        QueueSubmittedWorkload(pWorkload, ++m_lastSubmittedSerial);
        lastSubmissionSerial = pWorkload->m_submissionSerial;
    }

    return submissionResult;
}

void VulkanQueue::MergeWorkloads(const HeapVector<VulkanWorkload*>& workloadsToSubmit,
                                 WorkloadMergeResult& outMergeResult)
{
    outMergeResult.workloadsToSubmit.reserve(workloadsToSubmit.size());

    for (VulkanWorkload* pWorkload : workloadsToSubmit)
    {
        if (!outMergeResult.workloadsToSubmit.empty() &&
            CanMergeWorkloads(outMergeResult.workloadsToSubmit.back(), pWorkload))
        {
            outMergeResult.workloadsToSubmit.back()->Merge(pWorkload);
            continue;
        }

        outMergeResult.workloadsToSubmit.push_back(pWorkload);
    }

    uint64_t nextSerial = m_lastSubmittedSerial;

    for (VulkanWorkload* pMergedWorkload : outMergeResult.workloadsToSubmit)
    {
        const uint64_t submissionSerial     = ++nextSerial;
        pMergedWorkload->m_submissionSerial = submissionSerial;

        outMergeResult.totalWaitSemaphoreCount += pMergedWorkload->m_waitSemaphoreInfos.size();
        outMergeResult.totalSignalSemaphoreCount +=
            pMergedWorkload->m_signalSemaphoreInfos.size() + 1;
        outMergeResult.totalCommandBufferCount += pMergedWorkload->m_commandBuffers.size();
    }

    for (VulkanWorkload* pWorkload : workloadsToSubmit)
    {
        if (pWorkload->m_pMergedInto != nullptr)
        {
            pWorkload->m_submissionSerial = pWorkload->m_pMergedInto->m_submissionSerial;
        }
    }
}

void VulkanQueue::BuildTimelineSubmitBatch(const WorkloadMergeResult& mergeResult,
                                           TimelineSubmitBatch& outSubmitBatch)
{
    outSubmitBatch.submitInfos.reserve(mergeResult.workloadsToSubmit.size());
    outSubmitBatch.timelineSubmitInfos.reserve(mergeResult.workloadsToSubmit.size());
    outSubmitBatch.commandBuffers.reserve(mergeResult.totalCommandBufferCount);
    outSubmitBatch.waitSemaphores.reserve(mergeResult.totalWaitSemaphoreCount);
    outSubmitBatch.signalSemaphores.reserve(mergeResult.totalSignalSemaphoreCount);
    outSubmitBatch.waitStageMasks.reserve(mergeResult.totalWaitSemaphoreCount);
    outSubmitBatch.waitSemaphoreValues.reserve(mergeResult.totalWaitSemaphoreCount);
    outSubmitBatch.signalSemaphoreValues.reserve(mergeResult.totalSignalSemaphoreCount);

    for (VulkanWorkload* pWorkload : mergeResult.workloadsToSubmit)
    {
        AppendTimelineSubmitWorkload(pWorkload, outSubmitBatch);
    }
}

void VulkanQueue::AppendTimelineSubmitWorkload(VulkanWorkload* pWorkload,
                                               TimelineSubmitBatch& outSubmitBatch)
{
    const size_t firstWaitSemaphoreIndex   = outSubmitBatch.waitSemaphores.size();
    const size_t firstWaitValueIndex       = outSubmitBatch.waitSemaphoreValues.size();
    const size_t firstSignalSemaphoreIndex = outSubmitBatch.signalSemaphores.size();
    const size_t firstSignalValueIndex     = outSubmitBatch.signalSemaphoreValues.size();
    const size_t firstCommandBufferIndex   = outSubmitBatch.commandBuffers.size();

    for (VulkanWorkload::WaitSemaphoreInfo const& waitInfo : pWorkload->m_waitSemaphoreInfos)
    {
        outSubmitBatch.waitSemaphores.push_back(waitInfo.pSemaphore->GetVkHandle());
        outSubmitBatch.waitStageMasks.push_back(waitInfo.waitFlags);
        outSubmitBatch.waitSemaphoreValues.push_back(waitInfo.value);
    }

    for (FVulkanCommandBuffer* pCmdBuffer : pWorkload->m_commandBuffers)
    {
        outSubmitBatch.commandBuffers.push_back(pCmdBuffer->GetVkHandle());
    }

    for (VulkanWorkload::SignalSemaphoreInfo const& signalInfo : pWorkload->m_signalSemaphoreInfos)
    {
        outSubmitBatch.signalSemaphores.push_back(signalInfo.pSemaphore->GetVkHandle());
        outSubmitBatch.signalSemaphoreValues.push_back(signalInfo.value);
    }

    outSubmitBatch.signalSemaphores.push_back(m_pTimelineSemaphore->GetVkHandle());
    outSubmitBatch.signalSemaphoreValues.push_back(pWorkload->m_submissionSerial);

    VkTimelineSemaphoreSubmitInfo timelineSubmitInfo;
    InitVkStruct(timelineSubmitInfo, VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO);
    timelineSubmitInfo.waitSemaphoreValueCount =
        static_cast<uint32_t>(outSubmitBatch.waitSemaphoreValues.size() - firstWaitValueIndex);
    timelineSubmitInfo.pWaitSemaphoreValues = timelineSubmitInfo.waitSemaphoreValueCount > 0 ?
        outSubmitBatch.waitSemaphoreValues.data() + firstWaitValueIndex :
        nullptr;
    timelineSubmitInfo.signalSemaphoreValueCount =
        static_cast<uint32_t>(outSubmitBatch.signalSemaphoreValues.size() - firstSignalValueIndex);
    timelineSubmitInfo.pSignalSemaphoreValues = timelineSubmitInfo.signalSemaphoreValueCount > 0 ?
        outSubmitBatch.signalSemaphoreValues.data() + firstSignalValueIndex :
        nullptr;
    outSubmitBatch.timelineSubmitInfos.emplace_back(timelineSubmitInfo);

    VkSubmitInfo submitInfo;
    InitVkStruct(submitInfo, VK_STRUCTURE_TYPE_SUBMIT_INFO);
    submitInfo.pNext = &outSubmitBatch.timelineSubmitInfos[outSubmitBatch.submitInfos.size()];
    submitInfo.waitSemaphoreCount =
        static_cast<uint32_t>(outSubmitBatch.waitSemaphores.size() - firstWaitSemaphoreIndex);
    submitInfo.pWaitSemaphores   = submitInfo.waitSemaphoreCount > 0 ?
        outSubmitBatch.waitSemaphores.data() + firstWaitSemaphoreIndex :
        nullptr;
    submitInfo.pWaitDstStageMask = submitInfo.waitSemaphoreCount > 0 ?
        outSubmitBatch.waitStageMasks.data() + firstWaitSemaphoreIndex :
        nullptr;
    submitInfo.commandBufferCount =
        static_cast<uint32_t>(outSubmitBatch.commandBuffers.size() - firstCommandBufferIndex);
    submitInfo.pCommandBuffers = submitInfo.commandBufferCount > 0 ?
        outSubmitBatch.commandBuffers.data() + firstCommandBufferIndex :
        nullptr;
    submitInfo.signalSemaphoreCount =
        static_cast<uint32_t>(outSubmitBatch.signalSemaphores.size() - firstSignalSemaphoreIndex);
    submitInfo.pSignalSemaphores = submitInfo.signalSemaphoreCount > 0 ?
        outSubmitBatch.signalSemaphores.data() + firstSignalSemaphoreIndex :
        nullptr;
    outSubmitBatch.submitInfos.emplace_back(submitInfo);
}

void VulkanQueue::QueueSubmittedWorkload(VulkanWorkload* pWorkload, uint64_t submissionSerial)
{
    pWorkload->m_submissionSerial = submissionSerial;
    // One acceptance path for every tracked resource, including merged recordings.
    GVulkanRHI->GetLifetimeTracker().SubmitRecordings(pWorkload->m_lifetimeIds, this,
                                                      submissionSerial);
    pWorkload->m_lifetimeIds.clear();
    // Only called after vkQueueSubmit succeeds. Merging already transfers all
    // signal entries to the root, so no separate receipt or child traversal is needed.
    for (const VulkanWorkload::SignalSemaphoreInfo& signal : pWorkload->m_signalSemaphoreInfos)
    {
        signal.pSemaphore->m_pSignalQueue           = this;
        signal.pSemaphore->m_signalSubmissionSerial = submissionSerial;
        ++signal.pSemaphore->m_signalGeneration;
    }
    for (FVulkanCommandBuffer* pCmdBuffer : pWorkload->m_commandBuffers)
    {
        pCmdBuffer->SetSubmitted();
    }
    m_workloadsPendingProcess.Push(pWorkload);
}

RHISubmissionResult VulkanQueue::SubmitWorkloadsWithTimelineSemaphore(
    uint64_t& lastSubmissionSerial)
{
    RHISubmissionResult returnValue{};

    HeapVector<VulkanWorkload*> workloadsToSubmit;
    workloadsToSubmit.reserve(m_workloadsPendingSubmit.Size());

    while (!m_workloadsPendingSubmit.Empty())
    {
        VulkanWorkload* pWorkload = m_workloadsPendingSubmit.Peek();
        m_workloadsPendingSubmit.Pop();
        workloadsToSubmit.push_back(pWorkload);
    }

    if (workloadsToSubmit.empty())
    {
        returnValue = RHISubmissionResult::eSuccess;
    }
    else
    {
        WorkloadMergeResult mergeResult;
        MergeWorkloads(workloadsToSubmit, mergeResult);

        TimelineSubmitBatch submitBatch;
        BuildTimelineSubmitBatch(mergeResult, submitBatch);

        const VkResult result =
            vkQueueSubmit(m_handle, static_cast<uint32_t>(submitBatch.submitInfos.size()),
                          submitBatch.submitInfos.data(), VK_NULL_HANDLE);

        if (result != VK_SUCCESS)
        {
            for (VulkanWorkload* workload : workloadsToSubmit)
            {
                workload->m_submissionSerial = 0;
            }

            // Keep roots (which own merged children) until platform/context references are consumed.
            for (VulkanWorkload* workload : mergeResult.workloadsToSubmit)
            {
                m_workloadsPendingSubmit.Push(workload);
            }

            returnValue = SubmissionFailure(result, false);
        }
        else
        {
            lastSubmissionSerial  = workloadsToSubmit.back()->m_submissionSerial;
            m_lastSubmittedSerial = lastSubmissionSerial;
            // Roots own merged children; enqueue each owned tree once for completion/reclamation.
            for (VulkanWorkload* pWorkload : mergeResult.workloadsToSubmit)
            {
                QueueSubmittedWorkload(pWorkload, pWorkload->m_submissionSerial);
            }

            returnValue = RHISubmissionResult::eSuccess;
        }
    }

    return returnValue;
}

RHISubmissionResult VulkanQueue::SubmitPendingWorkloads(uint64_t& serial)
{
    RHISubmissionResult result{};

    serial = 0;
    // Retire previously completed submissions before appending new ones. This keeps the pending
    // queue bounded to in-flight GPU work instead of growing for the whole app lifetime.
    ProcessPendingWorkloads(0);

    if (GVulkanRHI->AreSubmissionsBlocked())
    {
        result = RHISubmissionResult::eFatal;
    }
    else if (m_workloadsPendingSubmit.Empty())
    {
        result = RHISubmissionResult::eSuccess;
    }
    else
    {
        // Fences can only be attached once per vkQueueSubmit call, so the non-timeline path still
        // needs one queue submit per workload to preserve per-workload completion tracking.
        if (!m_pDevice->SupportsTimelineSemaphore())
        {
            result = SubmitWorkloadsWithFences(serial);
        }
        else
        {
            result = SubmitWorkloadsWithTimelineSemaphore(serial);
        }
    }

    return result;
}

void VulkanQueue::ProcessPendingWorkloads(uint64_t timeToWaitNS, uint64_t maxSubmissionSerial)
{
    const std::chrono::steady_clock::time_point start =
        timeToWaitNS != 0 && timeToWaitNS != UINT64_MAX ? std::chrono::steady_clock::now() :
                                                          std::chrono::steady_clock::time_point{};
    HeapVector<FVulkanCommandBufferPool*> cmdBufferPoolsToTrim;

    while (!GVulkanRHI->AreSubmissionsBlocked())
    {
        if (m_workloadsPendingProcess.Empty())
        {
            break;
        }

        VulkanWorkload* pWorkload = m_workloadsPendingProcess.Peek();

        if (pWorkload->m_submissionSerial > maxSubmissionSerial)
        {
            break;
        }

        // A finite timeout covers the entire call, including preceding fence submissions.
        uint64_t remainingNS = timeToWaitNS;

        if (timeToWaitNS != UINT64_MAX && timeToWaitNS != 0)
        {
            const uint64_t elapsed = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                  std::chrono::steady_clock::now() - start)
                                                  .count());
            remainingNS            = elapsed < timeToWaitNS ? timeToWaitNS - elapsed : 0;
        }

        VkResult result = VK_NOT_READY;

        if (m_pDevice->SupportsTimelineSemaphore())
        {
            uint64_t completedValue = 0;
            result                  = m_pTimelineSemaphore->GetCounterValue(completedValue);
            if (result == VK_SUCCESS && completedValue < pWorkload->m_submissionSerial)
            {
                result = remainingNS == 0 ?
                    VK_NOT_READY :
                    m_pTimelineSemaphore->Wait(pWorkload->m_submissionSerial, remainingNS);
                if (result == VK_SUCCESS)
                {
                    // The successful wait proves this serial completed without another query.
                    completedValue = pWorkload->m_submissionSerial;
                }
            }
            if (result == VK_SUCCESS)
            {
                m_lastCompletedSerial = std::max(m_lastCompletedSerial.load(), completedValue);
            }
        }
        else
        {
            VulkanFence* pFence               = pWorkload->m_pFence;
            VulkanFenceManager* pFenceManager = pFence->GetOwner();
            result = remainingNS == 0 ? pFenceManager->GetFenceStatus(pFence) :
                                        pFenceManager->WaitForFence(pFence, remainingNS);
        }

        if (result != VK_SUCCESS)
        {
            if (result != VK_NOT_READY && result != VK_TIMEOUT)
            {
                LOGE("Vulkan queue {} completion query/wait failed: {}", m_familyIndex,
                     int32_t(result));
                GVulkanRHI->BlockSubmissions();
            }
            // Incomplete work and failures both retain ownership; only failures are terminal.
            break;
        }

        m_workloadsPendingProcess.Pop();

        for (FVulkanCommandBuffer* pCmdBuffer : pWorkload->m_commandBuffers)
        {
            pCmdBuffer->SetCompleted();
            AppendCommandBufferPool(cmdBufferPoolsToTrim, pCmdBuffer->GetCommandBufferPool());
        }

        if (m_lastCompletedSerial < pWorkload->m_submissionSerial)
        {
            m_lastCompletedSerial = pWorkload->m_submissionSerial;
        }

        ReleaseWorkload(pWorkload);
    }

    // Trim once per pool after retiring all currently completed workloads.
    for (FVulkanCommandBufferPool* pCmdBufferPool : cmdBufferPoolsToTrim)
    {
        pCmdBufferPool->FreeUnusedCommandBuffers();
    }
    GVulkanRHI->GetLifetimeTracker().Collect();
}

bool VulkanQueue::WaitForCompletion(uint64_t submissionSerial, uint64_t timeToWaitNS)
{
    bool result{};

    if (submissionSerial > m_lastSubmittedSerial)
    {
        LOGE("Vulkan queue {}: cannot wait for unsubmitted serial {} (last submitted {})",
             m_familyIndex, submissionSerial, m_lastSubmittedSerial);

        result = false;
    }
    else if (submissionSerial == 0 || m_lastCompletedSerial >= submissionSerial)
    {
        result = true;
    }
    else
    {
        ProcessPendingWorkloads(timeToWaitNS, submissionSerial);
        result = m_lastCompletedSerial >= submissionSerial;
    }

    return result;
}
} // namespace zen
