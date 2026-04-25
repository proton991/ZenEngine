#include "Graphics/VulkanRHI/VulkanQueue.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanSynchronization.h"
#include "Graphics/VulkanRHI/VulkanCommandBuffer.h"
#include "Graphics/VulkanRHI/VulkanCommandList.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"

namespace zen
{
VulkanQueue::VulkanQueue(VulkanDevice* pDevice, uint32_t familyIndex) :
    m_pDevice(pDevice), m_familyIndex(familyIndex), m_queueIndex(0)
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
    auto it = m_cmdBufferPools.end();
    while (it != m_cmdBufferPools.begin())
    {
        --it;
        FVulkanCommandBufferPool* pCmdBufferPool = *it;
        if (pCmdBufferPool->GetCommandBufferType() == type)
        {
            m_cmdBufferPools.erase(it);
            return pCmdBufferPool;
        }
    }

    return ZEN_NEW() FVulkanCommandBufferPool(this, type);
}

void VulkanQueue::RecycleCommandBufferPool(FVulkanCommandBufferPool* pCmdBufferPool)
{
    VERIFY_EXPR(pCmdBufferPool->GetQueue() == this);
    pCmdBufferPool->FreeUnusedCommandBuffers();
    m_cmdBufferPools.emplace_back(pCmdBufferPool);
}

void VulkanQueue::Submit(VulkanCommandBuffer* pCmdBuffer,
                         uint32_t numSignalSemaphores,
                         VkSemaphore* pSignalSemaphores)
{
    VulkanFence* pFence = pCmdBuffer->m_pFence;
    VERIFY_EXPR(!pFence->IsSignaled());

    const VkCommandBuffer vkCommandBuffer[] = {pCmdBuffer->GetVkHandle()};
    VkSubmitInfo submitInfo;
    InitVkStruct(submitInfo, VK_STRUCTURE_TYPE_SUBMIT_INFO);
    submitInfo.commandBufferCount   = 1;
    submitInfo.pCommandBuffers      = vkCommandBuffer;
    submitInfo.signalSemaphoreCount = numSignalSemaphores;
    submitInfo.pSignalSemaphores    = pSignalSemaphores;

    HeapVector<VkSemaphore> waitSemaphores;
    if (!pCmdBuffer->m_waitSemaphores.empty())
    {
        waitSemaphores.reserve(pCmdBuffer->m_waitSemaphores.size());
        for (VulkanSemaphore* pSem : pCmdBuffer->m_waitSemaphores)
        {
            waitSemaphores.push_back(pSem->GetVkHandle());
        }
        submitInfo.waitSemaphoreCount = waitSemaphores.size();
        submitInfo.pWaitSemaphores    = waitSemaphores.data();
        submitInfo.pWaitDstStageMask  = pCmdBuffer->m_waitFlags.data();
    }
    VKCHECK(vkQueueSubmit(m_handle, 1, &submitInfo, pFence->GetVkHandle()));

    pCmdBuffer->m_state = VulkanCommandBuffer::State::eSubmitted;
    pCmdBuffer->MarkSemaphoresAsSubmitted();

    UpdateLastSubmittedCmdBuffer(pCmdBuffer);

    pCmdBuffer->m_pCmdBufferPool->RefreshFenceStatus(pCmdBuffer);
}

void VulkanQueue::Submit(VulkanCommandBuffer* pCmdBuffer, VkSemaphore signalSemaphore)
{
    Submit(pCmdBuffer, 1, &signalSemaphore);
}

void VulkanQueue::Submit(VulkanCommandBuffer* pCmdBuffer)
{
    Submit(pCmdBuffer, 0, nullptr);
}

void VulkanQueue::GetLastSubmitInfo(VulkanCommandBuffer*& cmdBuffer,
                                    uint64_t* pFenceSignaledCounter) const
{
    cmdBuffer              = m_pLastSubmittedCmdBuffer;
    *pFenceSignaledCounter = m_pLastSubmittedCmdBuffer->GetFenceSignaledCounter();
}

void VulkanQueue::UpdateLastSubmittedCmdBuffer(VulkanCommandBuffer* pCmdBuffer)
{
    m_pLastSubmittedCmdBuffer = pCmdBuffer;
}

VulkanWorkload* VulkanQueue::AcquireWorkload()
{
    if (!m_workloadPool.empty())
    {
        VulkanWorkload* pWorkload = m_workloadPool.back();
        m_workloadPool.pop_back();
        return pWorkload;
    }

    return ZEN_NEW() VulkanWorkload(this);
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
    pWorkload->m_submissionSerial = 0;
    pWorkload->m_pMergedInto      = nullptr;
    pWorkload->m_waitSemaphoreInfos.clear();
    pWorkload->m_signalSemaphoreInfos.clear();
    m_workloadPool.push_back(pWorkload);
}

void VulkanQueue::DestroyWorkload(VulkanWorkload* pWorkload)
{
    if (pWorkload == nullptr)
    {
        return;
    }

    if (pWorkload->m_pFence != nullptr)
    {
        pWorkload->m_pFence->GetOwner()->ReleaseFence(pWorkload->m_pFence);
        pWorkload->m_pFence = nullptr;
    }

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

uint64_t VulkanQueue::SubmitWorkloadsWithFences()
{
    if (m_workloadsPendingSubmit.Empty())
    {
        return 0;
    }

    uint64_t lastSubmissionSerial     = 0;
    VulkanFenceManager* pFenceManager = GVulkanRHI->GetDevice()->GetFenceManager();
    while (!m_workloadsPendingSubmit.Empty())
    {
        VulkanWorkload* pWorkload = m_workloadsPendingSubmit.Peek();
        m_workloadsPendingSubmit.Pop();

        pWorkload->m_submissionSerial = ++m_nextSubmissionSerial;
        pWorkload->m_pFence           = pFenceManager->CreateFence();
        lastSubmissionSerial          = pWorkload->m_submissionSerial;

        VkSubmitInfo submitInfo;
        InitVkStruct(submitInfo, VK_STRUCTURE_TYPE_SUBMIT_INFO);
        HeapVector<VkSemaphore> waitSemaphores;
        HeapVector<VkSemaphore> signalSemaphores;
        HeapVector<VkPipelineStageFlags> waitStageMasks;
        waitSemaphores.reserve(pWorkload->m_waitSemaphoreInfos.size());
        signalSemaphores.reserve(pWorkload->m_signalSemaphoreInfos.size());
        waitStageMasks.reserve(pWorkload->m_waitSemaphoreInfos.size());

        for (const auto& waitInfo : pWorkload->m_waitSemaphoreInfos)
        {
            waitSemaphores.push_back(waitInfo.pSemaphore->GetVkHandle());
            waitStageMasks.push_back(waitInfo.waitFlags);
        }
        for (const auto& signalInfo : pWorkload->m_signalSemaphoreInfos)
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

        VKCHECK(vkQueueSubmit(m_handle, 1, &submitInfo, submitFence));
        for (FVulkanCommandBuffer* pCmdBuffer : pWorkload->m_commandBuffers)
        {
            pCmdBuffer->SetSubmitted();
        }

        m_workloadsPendingProcess.Push(pWorkload);
    }

    return lastSubmissionSerial;
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

    for (VulkanWorkload* pMergedWorkload : outMergeResult.workloadsToSubmit)
    {
        const uint64_t submissionSerial     = ++m_nextSubmissionSerial;
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

    for (const auto& waitInfo : pWorkload->m_waitSemaphoreInfos)
    {
        outSubmitBatch.waitSemaphores.push_back(waitInfo.pSemaphore->GetVkHandle());
        outSubmitBatch.waitStageMasks.push_back(waitInfo.waitFlags);
        outSubmitBatch.waitSemaphoreValues.push_back(waitInfo.value);
    }

    for (FVulkanCommandBuffer* pCmdBuffer : pWorkload->m_commandBuffers)
    {
        outSubmitBatch.commandBuffers.push_back(pCmdBuffer->GetVkHandle());
    }

    for (const auto& signalInfo : pWorkload->m_signalSemaphoreInfos)
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

void VulkanQueue::QueueSubmittedWorkloads(const HeapVector<VulkanWorkload*>& workloadsToSubmit)
{
    for (VulkanWorkload* pWorkload : workloadsToSubmit)
    {
        for (FVulkanCommandBuffer* pCmdBuffer : pWorkload->m_commandBuffers)
        {
            pCmdBuffer->SetSubmitted();
        }
        m_workloadsPendingProcess.Push(pWorkload);
    }
}

uint64_t VulkanQueue::SubmitWorkloadsWithTimelineSemaphore()
{
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
        return 0;
    }

    WorkloadMergeResult mergeResult;
    MergeWorkloads(workloadsToSubmit, mergeResult);

    TimelineSubmitBatch submitBatch;
    BuildTimelineSubmitBatch(mergeResult, submitBatch);

    VKCHECK(vkQueueSubmit(m_handle, static_cast<uint32_t>(submitBatch.submitInfos.size()),
                          submitBatch.submitInfos.data(), VK_NULL_HANDLE));
    QueueSubmittedWorkloads(workloadsToSubmit);

    return workloadsToSubmit.back()->m_submissionSerial;
}

uint64_t VulkanQueue::SubmitPendingWorkloads()
{
    // Retire previously completed submissions before appending new ones. This keeps the pending
    // queue bounded to in-flight GPU work instead of growing for the whole app lifetime.
    ProcessPendingWorkloads(0);
    if (m_workloadsPendingSubmit.Empty())
    {
        return 0;
    }

    // Fences can only be attached once per vkQueueSubmit call, so the non-timeline path still
    // needs one queue submit per workload to preserve per-workload completion tracking.
    if (!m_pDevice->SupportsTimelineSemaphore())
    {
        return SubmitWorkloadsWithFences();
    }

    return SubmitWorkloadsWithTimelineSemaphore();
}

void VulkanQueue::ProcessPendingWorkloads(uint64_t timeToWaitNS)
{
    VulkanFenceManager* pFenceManager = GVulkanRHI->GetDevice()->GetFenceManager();
    HeapVector<FVulkanCommandBufferPool*> cmdBufferPoolsToTrim;

    auto AddCmdBufferPoolToTrim =
        [&cmdBufferPoolsToTrim](FVulkanCommandBufferPool* pCmdBufferPool) {
            if (pCmdBufferPool == nullptr)
            {
                return;
            }

            for (FVulkanCommandBufferPool* pExistingPool : cmdBufferPoolsToTrim)
            {
                if (pExistingPool == pCmdBufferPool)
                {
                    return;
                }
            }

            cmdBufferPoolsToTrim.push_back(pCmdBufferPool);
        };

    while (true)
    {
        if (m_workloadsPendingProcess.Empty())
        {
            break;
        }

        VulkanWorkload* pWorkload = m_workloadsPendingProcess.Peek();
        bool success              = false;
        if (m_pDevice->SupportsTimelineSemaphore())
        {
            uint64_t completedValue = m_pTimelineSemaphore->GetCounterValue();
            if (completedValue >= pWorkload->m_submissionSerial)
            {
                m_lastCompletedSubmissionSerial = completedValue;
                success                         = true;
            }
            else if (timeToWaitNS != 0 &&
                     m_pTimelineSemaphore->Wait(pWorkload->m_submissionSerial, timeToWaitNS))
            {
                m_lastCompletedSubmissionSerial = m_pTimelineSemaphore->GetCounterValue();
                success = m_lastCompletedSubmissionSerial >= pWorkload->m_submissionSerial;
            }
        }
        else
        {
            VulkanFence* pFence = pWorkload->m_pFence;
            success             = timeToWaitNS == 0 ? pFenceManager->IsFenceSignaled(pFence) :
                                                      pFenceManager->WaitForFence(pFence, timeToWaitNS);
        }

        if (!success)
        {
            if (timeToWaitNS == 0)
            {
                break;
            }
            continue;
        }

        m_workloadsPendingProcess.Pop();
        for (FVulkanCommandBuffer* pCmdBuffer : pWorkload->m_commandBuffers)
        {
            pCmdBuffer->SetCompleted();
            AddCmdBufferPoolToTrim(pCmdBuffer->GetCommandBufferPool());
        }
        if (m_lastCompletedSubmissionSerial < pWorkload->m_submissionSerial)
        {
            m_lastCompletedSubmissionSerial = pWorkload->m_submissionSerial;
        }
        ReleaseWorkload(pWorkload);
    }

    // Trim once per pool after retiring all currently completed workloads.
    for (FVulkanCommandBufferPool* pCmdBufferPool : cmdBufferPoolsToTrim)
    {
        pCmdBufferPool->FreeUnusedCommandBuffers();
    }
}

void VulkanQueue::WaitForSubmission(uint64_t submissionSerial, uint64_t timeToWaitNS)
{
    if (submissionSerial == 0 || m_lastCompletedSubmissionSerial >= submissionSerial)
    {
        return;
    }

    while (m_lastCompletedSubmissionSerial < submissionSerial)
    {
        ProcessPendingWorkloads(timeToWaitNS);
    }
}
} // namespace zen
