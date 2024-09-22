#include "Graphics/VulkanRHI/VulkanQueue.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanSynchronization.h"
#include "Graphics/VulkanRHI/VulkanCommandBuffer.h"

namespace zen::rhi
{
VulkanQueue::VulkanQueue(VulkanDevice* device, uint32_t familyIndex) :
    m_device(device), m_familyIndex(familyIndex), m_queueIndex(0)
{
    vkGetDeviceQueue(m_device->GetVkHandle(), m_familyIndex, m_queueIndex, &m_handle);
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

    std::vector<VkSemaphore> waitSemaphores;
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
} // namespace zen::rhi