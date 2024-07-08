#include "Graphics/RenderCore/RenderContext.h"
#include "Graphics/RenderCore/RenderConfig.h"
#include "Graphics/Val/Queue.h"
#include "Graphics/Val/CommandBuffer.h"
#include "Common/Errors.h"

namespace zen
{
RenderContext::RenderContext(const val::Device& device, platform::GlfwWindowImpl* window) :
    m_valDevice(device),
    m_queue(m_valDevice.GetQueue(val::QueueType::QUEUE_INDEX_GRAPHICS)),
    m_synObjPool(device),
    m_threadCount(RenderConfig::GetInstance().numThreads)
{
    m_surface   = window->CreateSurface(device.GetInstanceHandle());
    m_swapchain = MakeUnique<val::Swapchain>(m_valDevice, m_surface, window->GetExtent2D());
    Init();
}

RenderContext::~RenderContext()
{
    if (m_swapchain)
    {
        vkDestroySwapchainKHR(m_valDevice.GetHandle(), m_swapchain->GetHandle(), nullptr);
    }
    if (m_surface != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(m_valDevice.GetInstanceHandle(), m_surface, nullptr);
    }
}

void RenderContext::Init()
{
    for (auto& imageHandle : m_swapchain->GetImages())
    {
        auto image = MakeUnique<val::Image>(m_valDevice, imageHandle, m_swapchain->GetExtent3D(),
                                            m_swapchain->GetFormat());
        m_frames.emplace_back(
            MakeUnique<RenderFrame>(m_valDevice, std::move(image), m_threadCount));
    }
    val::CommandPool::CreateInfo cmdPoolCI{};
    cmdPoolCI.queueFamilyIndex = m_queue.GetFamilyIndex();
    cmdPoolCI.resetMode        = val::CommandPool::ResetMode::ResetBuffer;
    // create common command pool
    m_commandPool = MakeUnique<val::CommandPool>(m_valDevice, cmdPoolCI);
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
    m_activeCmdBuffer = GetActiveFrame().RequestCommandBuffer(m_queue.GetFamilyIndex(), resetMode);
    m_activeCmdBuffer->Begin();
    return m_activeCmdBuffer;
}

void RenderContext::SubmitInternal()
{
    m_renderFinished = GetActiveFrame().RequestSemaphore();

    VkCommandBuffer handle = m_activeCmdBuffer->GetHandle();
    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &handle;

    VkPipelineStageFlags waitPipelineStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (m_imageAcquiredSem != VK_NULL_HANDLE)
    {
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores    = &m_imageAcquiredSem;
        submitInfo.pWaitDstStageMask  = &waitPipelineStage;
    }

    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores    = &m_renderFinished;
    // End command buffer before submit to a queue
    m_activeCmdBuffer->End();
    m_queue.Submit({submitInfo}, GetActiveFrame().RequestFence());
}

void RenderContext::EndFrame()
{
    ASSERT(m_frameActive);
    // change image layout to present
    VkImageMemoryBarrier transferDstToPresentBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    transferDstToPresentBarrier.srcAccessMask =
        val::Image::UsageToAccessFlags(val::ImageUsage::TransferDst);
    transferDstToPresentBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    transferDstToPresentBarrier.oldLayout =
        val::Image::UsageToImageLayout(val::ImageUsage::TransferDst);
    transferDstToPresentBarrier.newLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    transferDstToPresentBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    transferDstToPresentBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    transferDstToPresentBarrier.image = GetActiveFrame().GetSwapchainImage()->GetHandle();
    transferDstToPresentBarrier.subresourceRange =
        GetActiveFrame().GetSwapchainImage()->GetSubResourceRange();
    m_activeCmdBuffer->PipelineBarrier(
        val::Image::UsageToPipelineStage(val::ImageUsage::TransferDst),
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, {}, {transferDstToPresentBarrier});

    SubmitInternal();

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

        m_queue.Present(&presentInfo);
    }
    // set frame state
    if (m_imageAcquiredSem != VK_NULL_HANDLE)
    {
        GetActiveFrame().ReleaseSemaphoreWithOwnership(m_imageAcquiredSem);
        m_imageAcquiredSem = VK_NULL_HANDLE;
    }
    GetActiveFrame().Reset();
    m_frameActive     = false;
    m_activeCmdBuffer = nullptr;
}

void RenderContext::StartFrameInternal()
{
    ASSERT(!m_frameActive);
    ASSERT(m_activeFrameIndex < m_frames.size());

    auto& prevFrame    = m_frames[m_activeFrameIndex];
    m_imageAcquiredSem = prevFrame->RequestSemaphoreWithOwnership();
    if (m_swapchain)
    {
        auto result = m_swapchain->AcquireNextImage(m_activeFrameIndex, m_imageAcquiredSem);
        if (result != VK_SUCCESS)
        {
            prevFrame->ReleaseSemaphoreWithOwnership(m_imageAcquiredSem);
            prevFrame.Reset();
            return;
        }
    }
    m_frameActive = true;
}

/**
 * @brief Recreate Swapchain due to extent changes
 */
void RenderContext::RecreateSwapchain(uint32_t newWidth, uint32_t newHeight)
{
    m_valDevice.WaitIdle();
    VkSurfaceCapabilitiesKHR surfaceCaps;
    CHECK_VK_ERROR_AND_THROW(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
                                 m_valDevice.GetPhysicalDeviceHandle(), m_surface, &surfaceCaps),
                             "Failed to get physical surface caps");
    if (surfaceCaps.currentExtent.width == 0xFFFFFFFF)
    {
        return;
    }

    if (surfaceCaps.currentExtent.width != m_swapchain->GetExtent2D().width ||
        surfaceCaps.currentExtent.height != m_swapchain->GetExtent2D().height)
    {
        VkExtent2D newExtent = {std::clamp(newWidth, surfaceCaps.minImageExtent.width,
                                           surfaceCaps.maxImageExtent.width),
                                std::clamp(newHeight, surfaceCaps.minImageExtent.height,
                                           surfaceCaps.maxImageExtent.height)};

        m_swapchain =
            MakeUnique<val::Swapchain>(m_valDevice, m_surface, newExtent, m_swapchain->GetHandle());
        // recreate images
        m_frames.clear();
        for (auto& imageHandle : m_swapchain->GetImages())
        {
            auto image = MakeUnique<val::Image>(
                m_valDevice, imageHandle, m_swapchain->GetExtent3D(), m_swapchain->GetFormat());
            m_frames.emplace_back(
                MakeUnique<RenderFrame>(m_valDevice, std::move(image), m_threadCount));
        }
    }
}

void RenderContext::SubmitImmediate(val::CommandBuffer* pCmdBuffer)
{
    VkCommandBuffer handle = pCmdBuffer->GetHandle();
    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &handle;
    m_queue.Submit({submitInfo}, m_synObjPool.RequestFence());
    m_synObjPool.WaitForFences();
    m_synObjPool.ResetFences();
}
} // namespace zen