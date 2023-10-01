#include "Graphics/Rendering/RenderContext.h"
#include "Graphics/Val/Queue.h"
#include "Graphics/Val/CommandBuffer.h"
#include "Common/Errors.h"

namespace zen
{
RenderContext::RenderContext(val::Device& device, platform::GlfwWindowImpl* window) :
    m_valDevice(device), m_queue(m_valDevice.GetQueue(val::QueueType::QUEUE_INDEX_GRAPHICS))
{
    m_surface   = window->CreateSurface(device.GetInstanceHandle());
    m_swapchain = MakeUnique<val::Swapchain>(m_valDevice, m_surface, window->GetExtent2D());
    Init();
}

void RenderContext::Init()
{
    for (auto& imageHandle : m_swapchain->GetImages())
    {
        auto image = MakeUnique<val::Image>(m_valDevice, imageHandle, m_swapchain->GetExtent3D(), m_swapchain->GetFormat());
        auto frame = RenderFrame(m_valDevice, std::move(image));
        m_frames.push_back(std::move(frame));
    }
}

val::CommandBuffer* RenderContext::StartFrame(val::CommandPool::ResetMode resetMode)
{
    if (!m_frameActive)
    {
        StartFrameInternal();
    }
    if (!m_frameActive)
    {
        LOG_ERROR_AND_THROW("Failed to start frame");
    }
    m_commandBuffer = GetActiveFrame().RequestCommandBuffer(m_queue.GetFamilyIndex(), resetMode);
    return m_commandBuffer;
}

void RenderContext::Submit()
{
    ASSERT(m_frameActive);
    RenderFrame& currentFrame = GetActiveFrame();
    m_renderFinished          = currentFrame.RequestSemaphore();

    std::vector<VkCommandBuffer> cmdBufferHandles{m_commandBuffer->GetHandle()};

    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = cmdBufferHandles.data();

    VkPipelineStageFlags waitPipelineStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (m_imageAcquiredSem != VK_NULL_HANDLE)
    {
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores    = &m_imageAcquiredSem;
        submitInfo.pWaitDstStageMask  = &waitPipelineStage;
    }

    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores    = &m_renderFinished;

    m_queue.Submit({submitInfo}, currentFrame.RequestFence());
}

void RenderContext::EndFrame()
{
    // change image layout to present
    VkImageMemoryBarrier transferDstToPresentBarrier{};
    transferDstToPresentBarrier.srcAccessMask       = val::Image::UsageToAccessFlags(VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    transferDstToPresentBarrier.srcAccessMask       = VK_ACCESS_MEMORY_READ_BIT;
    transferDstToPresentBarrier.oldLayout           = val::Image::UsageToImageLayout(VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    transferDstToPresentBarrier.newLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    transferDstToPresentBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    transferDstToPresentBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    transferDstToPresentBarrier.image               = GetActiveFrame().GetSwapchainImage()->GetHandle();
    transferDstToPresentBarrier.subresourceRange    = GetActiveFrame().GetSwapchainImage()->GetSubResourceRange();
    m_commandBuffer->PipelineBarrier(val::Image::UsageToPipelineStage(VK_IMAGE_USAGE_TRANSFER_DST_BIT), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, {}, {transferDstToPresentBarrier});
    
    // present image
    ASSERT(m_frameActive);
    if (m_swapchain)
    {
        VkSwapchainKHR swapchainHandle = m_swapchain->GetHandle();

        VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores    = &m_renderFinished;
        presentInfo.swapchainCount     = 1;
        presentInfo.pSwapchains        = &swapchainHandle;
        presentInfo.pImageIndices      = &m_activeFrameIndex;

        VkResult result = m_queue.Present(&presentInfo);
        if (result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR)
        {
            RecreateSwapchain();
        }
    }
    // set frame state
    if (m_imageAcquiredSem != VK_NULL_HANDLE)
    {
        GetActiveFrame().ReleaseSemaphoreWithOwnership(m_imageAcquiredSem);
        m_imageAcquiredSem = VK_NULL_HANDLE;
    }
    GetActiveFrame().Reset();
    m_frameActive   = false;
    m_commandBuffer = nullptr;
}

void RenderContext::StartFrameInternal()
{
    ASSERT(!m_frameActive);
    ASSERT(m_activeFrameIndex < m_frames.size());

    auto& prevFrame    = m_frames[m_activeFrameIndex];
    m_imageAcquiredSem = prevFrame.RequestSemaphoreWithOwnership();
    if (m_swapchain)
    {
        auto result = m_swapchain->AcquireNextImage(m_activeFrameIndex, m_imageAcquiredSem);
        if (result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR)
        {
            RecreateSwapchain();
        }
        if (result != VK_SUCCESS)
        {
            prevFrame.ReleaseSemaphoreWithOwnership(m_imageAcquiredSem);
            prevFrame.Reset();
            return;
        }
    }
    m_frameActive = true;
}

/**
 * @brief Recreate Swapchain due to extent changes
 */
void RenderContext::RecreateSwapchain()
{
    VkSurfaceCapabilitiesKHR surfaceCaps;
    CHECK_VK_ERROR_AND_THROW(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_valDevice.GetPhysicalDeviceHandle(),
                                                                       m_surface,
                                                                       &surfaceCaps),
                             "Failed to get physical surface caps");
    if (surfaceCaps.currentExtent.width == 0xFFFFFFFF)
    {
        return;
    }

    if (surfaceCaps.currentExtent.width != m_swapchain->GetExtent2D().width || surfaceCaps.currentExtent.height != m_swapchain->GetExtent2D().height)
    {
        VkExtent2D newExtent = {surfaceCaps.currentExtent.width, surfaceCaps.currentExtent.height};
        m_swapchain          = MakeUnique<val::Swapchain>(m_valDevice, m_surface, newExtent);
        // recreate images
        m_frames.clear();
        Init();
    }
}
} // namespace zen