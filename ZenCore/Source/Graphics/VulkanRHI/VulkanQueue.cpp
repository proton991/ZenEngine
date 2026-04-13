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
        ZEN_DELETE(pWorkload);
    }

    while (!m_workloadsPendingProcess.Empty())
    {
        VulkanWorkload* pWorkload = m_workloadsPendingProcess.Peek();
        m_workloadsPendingProcess.Pop();
        ZEN_DELETE(pWorkload);
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
    cmdBuffer             = m_pLastSubmittedCmdBuffer;
    *pFenceSignaledCounter = m_pLastSubmittedCmdBuffer->GetFenceSignaledCounter();
}

void VulkanQueue::UpdateLastSubmittedCmdBuffer(VulkanCommandBuffer* pCmdBuffer)
{
    m_pLastSubmittedCmdBuffer = pCmdBuffer;
}

void VulkanQueue::SubmitWorkloads()
{
    const bool useTimelineSemaphore = m_pTimelineSemaphore != nullptr;

    // Retire previously completed submissions before appending new ones. This keeps the pending
    // queue bounded to in-flight GPU work instead of growing for the whole app lifetime.
    ProcessPendingWorkloads(0);

    // When using fences, submit workload one by one
    while (!m_workloadsPendingSubmit.Empty())
    {
        VulkanWorkload* pWorkload = m_workloadsPendingSubmit.Peek();
        m_workloadsPendingSubmit.Pop();
        pWorkload->m_submissionSerial = ++m_nextSubmissionSerial;
        if (!useTimelineSemaphore)
        {
            pWorkload->m_pFence = GVulkanRHI->GetDevice()->GetFenceManager()->CreateFence();
        }

        VkSubmitInfo submitInfo;
        InitVkStruct(submitInfo, VK_STRUCTURE_TYPE_SUBMIT_INFO);
        HeapVector<VkSemaphore> waitSemaphores;
        HeapVector<VkSemaphore> signalSemaphores;
        HeapVector<VkPipelineStageFlags> waitStageMasks;

        for (const auto& waitInfo : pWorkload->m_waitSemaphoreInfos)
        {
            waitSemaphores.push_back(waitInfo.pSemaphore->GetVkHandle());
            waitStageMasks.push_back(waitInfo.waitFlags);
        }
        for (const auto& signalInfo : pWorkload->m_signalSemaphoreInfos)
        {
            signalSemaphores.push_back(signalInfo.pSemaphore->GetVkHandle());
        }

        VkTimelineSemaphoreSubmitInfo timelineSubmitInfo;
        HeapVector<uint64_t> waitSemaphoreValues;
        HeapVector<uint64_t> signalSemaphoreValues;
        if (useTimelineSemaphore)
        {
            waitSemaphoreValues.reserve(pWorkload->m_waitSemaphoreInfos.size());
            signalSemaphoreValues.reserve(pWorkload->m_signalSemaphoreInfos.size() + 1);
            for (const auto& waitInfo : pWorkload->m_waitSemaphoreInfos)
            {
                waitSemaphoreValues.push_back(waitInfo.value);
            }
            for (const auto& signalInfo : pWorkload->m_signalSemaphoreInfos)
            {
                signalSemaphoreValues.push_back(signalInfo.value);
            }
            signalSemaphores.push_back(m_pTimelineSemaphore->GetVkHandle());
            signalSemaphoreValues.push_back(pWorkload->m_submissionSerial);

            InitVkStruct(timelineSubmitInfo, VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO);
            timelineSubmitInfo.waitSemaphoreValueCount   = waitSemaphoreValues.size();
            timelineSubmitInfo.pWaitSemaphoreValues      = waitSemaphoreValues.data();
            timelineSubmitInfo.signalSemaphoreValueCount = signalSemaphoreValues.size();
            timelineSubmitInfo.pSignalSemaphoreValues    = signalSemaphoreValues.data();

            submitInfo.pNext = &timelineSubmitInfo;
        }

        VkCommandBuffer cmdBuffer = pWorkload->m_pCmdBuffer->GetVkHandle();

        submitInfo.commandBufferCount   = 1;
        submitInfo.pCommandBuffers      = &cmdBuffer;
        submitInfo.signalSemaphoreCount = signalSemaphores.size();
        submitInfo.pSignalSemaphores    = signalSemaphores.data();
        submitInfo.waitSemaphoreCount   = waitSemaphores.size();
        submitInfo.pWaitSemaphores      = waitSemaphores.data();
        submitInfo.pWaitDstStageMask    = waitStageMasks.data();

        VkFence submitFence =
            pWorkload->m_pFence != nullptr ? pWorkload->m_pFence->GetVkHandle() : VK_NULL_HANDLE;

        VKCHECK(vkQueueSubmit(m_handle, 1, &submitInfo, submitFence));
        pWorkload->m_pCmdBuffer->SetSubmitted();

        m_workloadsPendingProcess.Push(pWorkload);
    }
}

void VulkanQueue::ProcessPendingWorkloads(uint64_t timeToWaitNS)
{
    VulkanFenceManager* pFenceManager = GVulkanRHI->GetDevice()->GetFenceManager();
    HeapVector<FVulkanCommandBufferPool*> cmdBufferPoolsToTrim;

    auto AddCmdBufferPoolToTrim = [&cmdBufferPoolsToTrim](FVulkanCommandBufferPool* pCmdBufferPool)
    {
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
        if (pWorkload->m_pCmdBuffer != nullptr)
        {
            pWorkload->m_pCmdBuffer->SetCompleted();
            AddCmdBufferPoolToTrim(pWorkload->m_pCmdBuffer->GetCommandBufferPool());
        }
        if (m_lastCompletedSubmissionSerial < pWorkload->m_submissionSerial)
        {
            m_lastCompletedSubmissionSerial = pWorkload->m_submissionSerial;
        }
        ZEN_DELETE(pWorkload);
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
