#include "Graphics/VulkanRHI/VulkanQueue.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanSynchronization.h"
#include "Graphics/VulkanRHI/VulkanCommandBuffer.h"
#include "Graphics/VulkanRHI/VulkanCommandList.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"

namespace zen
{
VulkanQueue::VulkanQueue(VulkanDevice* device, uint32_t familyIndex) :
    m_device(device), m_familyIndex(familyIndex), m_queueIndex(0)
{
    vkGetDeviceQueue(m_device->GetVkHandle(), m_familyIndex, m_queueIndex, &m_handle);
    if (m_device->SupportsTimelineSemaphore())
    {
        m_pTimelineSemaphore = ZEN_NEW() VulkanSemaphore(m_device, VK_SEMAPHORE_TYPE_TIMELINE, 0);
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
    FVulkanCommandBufferPool* result = nullptr;
    if (!m_cmdBufferPools.empty())
    {
        result = m_cmdBufferPools.back();
        m_cmdBufferPools.pop_back();
    }
    else
    {
        result = ZEN_NEW() FVulkanCommandBufferPool(this, type);
    }
    return result;
}

void VulkanQueue::RecycleCommandBufferPool(FVulkanCommandBufferPool* pCmdBufferPool)
{
    VERIFY_EXPR(pCmdBufferPool->GetQueue() == this);
    m_cmdBufferPools.emplace_back(pCmdBufferPool);
}

void VulkanQueue::Submit(VulkanCommandBuffer* cmdBuffer,
                         uint32_t numSignalSemaphores,
                         VkSemaphore* signalSemaphores)
{
    VulkanFence* fence = cmdBuffer->m_fence;
    VERIFY_EXPR(!fence->IsSignaled());

    const VkCommandBuffer vkCommandBuffer[] = {cmdBuffer->GetVkHandle()};
    VkSubmitInfo submitInfo;
    InitVkStruct(submitInfo, VK_STRUCTURE_TYPE_SUBMIT_INFO);
    submitInfo.commandBufferCount   = 1;
    submitInfo.pCommandBuffers      = vkCommandBuffer;
    submitInfo.signalSemaphoreCount = numSignalSemaphores;
    submitInfo.pSignalSemaphores    = signalSemaphores;

    HeapVector<VkSemaphore> waitSemaphores;
    if (!cmdBuffer->m_waitSemaphores.empty())
    {
        waitSemaphores.reserve(cmdBuffer->m_waitSemaphores.size());
        for (VulkanSemaphore* sem : cmdBuffer->m_waitSemaphores)
        {
            waitSemaphores.push_back(sem->GetVkHandle());
        }
        submitInfo.waitSemaphoreCount = waitSemaphores.size();
        submitInfo.pWaitSemaphores    = waitSemaphores.data();
        submitInfo.pWaitDstStageMask  = cmdBuffer->m_waitFlags.data();
    }
    VKCHECK(vkQueueSubmit(m_handle, 1, &submitInfo, fence->GetVkHandle()));

    cmdBuffer->m_state = VulkanCommandBuffer::State::eSubmitted;
    cmdBuffer->MarkSemaphoresAsSubmitted();

    UpdateLastSubmittedCmdBuffer(cmdBuffer);

    cmdBuffer->m_cmdBufferPool->RefreshFenceStatus(cmdBuffer);
}

void VulkanQueue::Submit(VulkanCommandBuffer* cmdBuffer, VkSemaphore signalSemaphore)
{
    Submit(cmdBuffer, 1, &signalSemaphore);
}

void VulkanQueue::Submit(VulkanCommandBuffer* cmdBuffer)
{
    Submit(cmdBuffer, 0, nullptr);
}

void VulkanQueue::GetLastSubmitInfo(VulkanCommandBuffer*& cmdBuffer,
                                    uint64_t* fenceSignaledCounter) const
{
    cmdBuffer             = m_lastSubmittedCmdBuffer;
    *fenceSignaledCounter = m_lastSubmittedCmdBuffer->GetFenceSignaledCounter();
}

void VulkanQueue::UpdateLastSubmittedCmdBuffer(VulkanCommandBuffer* cmdBuffer)
{
    m_lastSubmittedCmdBuffer = cmdBuffer;
}

void VulkanQueue::SubmitWorkloads()
{
    const bool useTimelineSemaphore = m_pTimelineSemaphore != nullptr;

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
            submitInfo.pNext                             = &timelineSubmitInfo;
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

    while (true)
    {
        if (m_workloadsPendingProcess.Empty())
        {
            break;
        }

        VulkanWorkload* pWorkload = m_workloadsPendingProcess.Peek();
        bool success              = false;
        if (m_device->SupportsTimelineSemaphore())
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
        if (m_lastCompletedSubmissionSerial < pWorkload->m_submissionSerial)
        {
            m_lastCompletedSubmissionSerial = pWorkload->m_submissionSerial;
        }
        ZEN_DELETE(pWorkload);
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
