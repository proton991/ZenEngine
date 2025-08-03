#include "Graphics/VulkanRHI/VulkanViewport.h"
#include "Common/Mutex.h"
#include "Graphics/RHI/RHIOptions.h"
#include "Graphics/VulkanRHI/VulkanTexture.h"
#include "Graphics/VulkanRHI/VulkanCommandBuffer.h"
#include "Graphics/VulkanRHI/VulkanCommands.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanQueue.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanRenderPass.h"
#include "Graphics/VulkanRHI/VulkanSynchronization.h"

namespace zen::rhi
{
RHIViewport* VulkanRHI::CreateViewport(void* windowPtr,
                                       uint32_t width,
                                       uint32_t height,
                                       bool enableVSync)
{
    RHIViewport* viewport = new VulkanViewport(this, windowPtr, width, height, enableVSync);
    return viewport;
}

void VulkanRHI::DestroyViewport(RHIViewport* viewport)
{
    dynamic_cast<VulkanViewport*>(viewport)->Destroy();
}

void VulkanRHI::BeginDrawingViewport(RHIViewport* viewportRHI)
{
    m_currentViewport = dynamic_cast<VulkanViewport*>(viewportRHI);
}

void VulkanRHI::EndDrawingViewport(RHIViewport* viewportRHI,
                                   RHICommandListContext* cmdListContext,
                                   bool present)
{
    VulkanViewport* viewport = dynamic_cast<VulkanViewport*>(viewportRHI);
    VERIFY_EXPR(viewport == m_currentViewport);
    if (present)
    {
        VulkanCommandBuffer* cmdBuffer = dynamic_cast<VulkanCommandListContext*>(cmdListContext)
                                             ->GetCmdBufferManager()
                                             ->GetActiveCommandBuffer();
        m_currentViewport->Present(cmdBuffer);
    }
}

VulkanViewport::VulkanViewport(VulkanRHI* RHI,
                               void* windowPtr,
                               uint32_t width,
                               uint32_t height,
                               bool enableVSync) :
    m_RHI(RHI),
    m_device(m_RHI->GetDevice()),
    m_windowPtr(windowPtr),
    m_width(width),
    m_height(height),
    m_enableVSync(enableVSync)
{
    m_depthFormat = m_RHI->GetSupportedDepthFormat();
    LOGI("Viewport backbuffer depth format: {}", VkToString(static_cast<VkFormat>(m_depthFormat)));
    CreateSwapchain(nullptr);
    for (uint32_t i = 0; i < m_renderingCompleteSemaphores.size(); i++)
    {
        auto* semaphore = m_RHI->GetDevice()->GetSemaphoreManager()->GetOrCreateSemaphore();
        const std::string debugName = "RenderComplete-" + std::to_string(i);
        semaphore->SetDebugName(debugName.c_str());
        m_renderingCompleteSemaphores[i] = semaphore;
    }
}

void VulkanViewport::WaitForFrameCompletion()
{
    if (RHIOptions::GetInstance().WaitForFrameCompletion())
    {
        static Mutex mutex;
        LockAuto lock(&mutex);
        if (m_lastFrameCmdBuffer && m_lastFrameCmdBuffer->IsSubmitted())
        {
            // last frame cmdbuffer fence not signaled, wait for it
            if (m_lastFenceSignaledCounter == m_lastFrameCmdBuffer->GetFenceSignaledCounter())
            {
                m_lastFrameCmdBuffer->GetOwner()->GetManager()->WaitForCmdBuffer(
                    m_lastFrameCmdBuffer);
            }
        }
    }
}

void VulkanViewport::IssueFrameEvent()
{
    if (RHIOptions::GetInstance().WaitForFrameCompletion())
    {
        m_RHI->GetDevice()->GetGfxQueue()->GetLastSubmitInfo(m_lastFrameCmdBuffer,
                                                             &m_lastFenceSignaledCounter);
    }
}

void VulkanViewport::CreateSwapchain(VulkanSwapchainRecreateInfo* recreateInfo)
{
    m_colorBackBuffer        = nullptr;
    m_depthStencilBackBuffer = nullptr;

    m_swapchain =
        new VulkanSwapchain(m_RHI, m_windowPtr, m_width, m_height, m_enableVSync, recreateInfo);
    const SmallVector<VkImage>& images = m_swapchain->GetSwapchainImages();
    m_renderingCompleteSemaphores.resize(images.size());
    m_backBufferImages.resize(images.size());

    VulkanCommandList* cmdList = dynamic_cast<VulkanCommandList*>(m_RHI->GetImmediateCommandList());
    cmdList->BeginUpload();
    VulkanCommandBuffer* cmdBuffer = cmdList->GetCmdBufferManager()->GetUploadCommandBuffer();

    const VkImageSubresourceRange range =
        VulkanTexture::GetSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);
    VkClearColorValue clearColor{0.1f, 0.1f, 0.1f, 1.0f};
    for (uint32_t i = 0; i < images.size(); i++)
    {
        m_backBufferImages[i] = images[i];
        cmdList->ChangeImageLayout(images[i], VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, range);
        vkCmdClearColorImage(cmdBuffer->GetVkHandle(), images[i],
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &range);
        cmdList->ChangeImageLayout(images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, range);
    }
    TextureInfo colorTexInfo{};
    colorTexInfo.width  = m_width;
    colorTexInfo.height = m_height;
    colorTexInfo.format = GetSwapchainFormat();
    colorTexInfo.usageFlags.SetFlag(TextureUsageFlagBits::eColorAttachment);
    colorTexInfo.usageFlags.SetFlag(TextureUsageFlagBits::eTransferSrc);
    colorTexInfo.type = TextureType::e2D;
    m_colorBackBuffer = TO_VK_TEXTURE(m_RHI->CreateTexture(colorTexInfo));

    TextureInfo depthStencilTexInfo{};
    depthStencilTexInfo.width  = m_width;
    depthStencilTexInfo.height = m_height;
    depthStencilTexInfo.format = GetDepthStencilFormat();
    depthStencilTexInfo.usageFlags.SetFlag(TextureUsageFlagBits::eDepthStencilAttachment);
    depthStencilTexInfo.type = TextureType::e2D;
    m_depthStencilBackBuffer = TO_VK_TEXTURE(m_RHI->CreateTexture(depthStencilTexInfo));
    // add image barriers, transfer back buffer layout
    VulkanPipelineBarrier barrier;
    barrier.AddImageBarrier(m_colorBackBuffer->image, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            VulkanTexture::GetSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT, 0, 1));
    barrier.AddImageBarrier(m_depthStencilBackBuffer->image, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                            VulkanTexture::GetSubresourceRange(
                                VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1));
    barrier.Execute(cmdBuffer);
    cmdList->EndUpload();

    m_acquiredImageIndex = -1;
}

void VulkanViewport::DestroySwapchain(VulkanSwapchainRecreateInfo* recreateInfo)
{
    m_RHI->GetDevice()->WaitForIdle();
    if (m_swapchain != nullptr)
    {
        for (uint32_t i = 0; i < m_backBufferImages.size(); i++)
        {
            m_backBufferImages[i] = VK_NULL_HANDLE;
        }
        m_swapchain->Destroy(recreateInfo);
        delete m_swapchain;
        m_swapchain = nullptr;
    }
    if (m_colorBackBuffer)
    {
        m_RHI->DestroyTexture(TextureHandle(m_colorBackBuffer));
        m_colorBackBuffer = nullptr;
    }
    if (m_depthStencilBackBuffer)
    {
        m_RHI->DestroyTexture(TextureHandle(m_depthStencilBackBuffer));
        m_depthStencilBackBuffer = nullptr;
    }
}

void VulkanViewport::RecreateSwapchain()
{
    VulkanSwapchainRecreateInfo recreateInfo{VK_NULL_HANDLE, VK_NULL_HANDLE};
    DestroySwapchain(&recreateInfo);
    CreateSwapchain(&recreateInfo);
    VERIFY_EXPR(recreateInfo.surface == VK_NULL_HANDLE);
    VERIFY_EXPR(recreateInfo.swapchain == VK_NULL_HANDLE);
}

bool VulkanViewport::TryAcquireNextImage()
{
    if (m_swapchain != nullptr)
    {
        int32_t result = m_swapchain->AcquireNextImage(&m_imageAcquiredSemaphore);
        if (result >= 0)
        {
            m_acquiredImageIndex = result;
            return true;
        }
    }
    return false;
}

void VulkanViewport::CopyToBackBufferForPresent(VulkanCommandBuffer* cmdBuffer,
                                                VkImage dstImage,
                                                uint32_t windowWidth,
                                                uint32_t windowHeight)
{
    const VkImageLayout prevLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    {
        VulkanPipelineBarrier barrier;
        barrier.AddImageBarrier(
            m_colorBackBuffer->image, prevLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VulkanTexture::GetSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1));
        barrier.AddImageBarrier(
            dstImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VulkanTexture::GetSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1));
        barrier.Execute(cmdBuffer);
    }
    if (m_width != windowWidth || m_height != windowHeight)
    {
        VkImageBlit region{};
        region.srcOffsets[0].x               = 0;
        region.srcOffsets[0].y               = 0;
        region.srcOffsets[0].z               = 0;
        region.srcOffsets[1].x               = static_cast<int32_t>(m_width);
        region.srcOffsets[1].y               = static_cast<int32_t>(m_height);
        region.srcOffsets[1].z               = 1;
        region.srcSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        region.srcSubresource.layerCount     = 1;
        region.srcSubresource.baseArrayLayer = 0;
        region.dstOffsets[0].x               = 0;
        region.dstOffsets[0].y               = 0;
        region.dstOffsets[0].z               = 0;
        region.dstOffsets[1].x               = static_cast<int32_t>(windowWidth);
        region.dstOffsets[1].y               = static_cast<int32_t>(windowHeight);
        region.dstOffsets[1].z               = 1;
        region.dstSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        region.dstSubresource.layerCount     = 1;
        region.dstSubresource.baseArrayLayer = 0;
        vkCmdBlitImage(cmdBuffer->GetVkHandle(), m_colorBackBuffer->image,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImage,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
    }
    else
    {
        VkImageCopy region{};
        region.extent.width                  = m_width;
        region.extent.height                 = m_height;
        region.extent.depth                  = 1;
        region.srcOffset.x                   = 0;
        region.srcOffset.y                   = 0;
        region.srcOffset.z                   = 0;
        region.dstOffset.x                   = 0;
        region.dstOffset.y                   = 0;
        region.dstOffset.z                   = 0;
        region.srcSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        region.srcSubresource.layerCount     = 1;
        region.srcSubresource.baseArrayLayer = 0;
        region.srcSubresource.mipLevel       = 0;
        region.dstSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        region.dstSubresource.layerCount     = 1;
        region.dstSubresource.baseArrayLayer = 0;
        region.dstSubresource.mipLevel       = 0;
        vkCmdCopyImage(cmdBuffer->GetVkHandle(), m_colorBackBuffer->image,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImage,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
    {
        VulkanPipelineBarrier barrier;
        barrier.AddImageBarrier(
            m_colorBackBuffer->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, prevLayout,
            VulkanTexture::GetSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1));
        barrier.AddImageBarrier(
            dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VulkanTexture::GetSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1));
        barrier.Execute(cmdBuffer);
    }
}

bool VulkanViewport::Present(VulkanCommandBuffer* cmdBuffer)
{
    bool acquireImageFailed = false;
    // VulkanCommandBuffer* cmdBuffer =
    //     m_device->GetImmediateCmdContext()->GetCmdBufferManager()->GetActiveCommandBuffer();
    if (TryAcquireNextImage())
    {
        uint32_t windowWidth  = std::min(m_width, m_swapchain->m_internalWidth);
        uint32_t windowHeight = std::min(m_height, m_swapchain->m_internalHeight);
        CopyToBackBufferForPresent(cmdBuffer, m_backBufferImages[m_acquiredImageIndex], windowWidth,
                                   windowHeight);
    }
    else
    {
        acquireImageFailed = true;
    }
    cmdBuffer->End();
    VulkanCommandBufferManager* cmdBufferMgr = cmdBuffer->GetOwner()->GetManager();
    // VulkanCommandBufferManager* cmdBufferMgr =
    //     m_device->GetImmediateCmdContext()->GetCmdBufferManager();
    if (!acquireImageFailed)
    {
        cmdBuffer->AddWaitSemaphore(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_imageAcquiredSemaphore);
        VulkanSemaphore* signalSemaphore = (m_acquiredImageIndex >= 0) ?
            m_renderingCompleteSemaphores[m_acquiredImageIndex] :
            nullptr;
        cmdBufferMgr->SubmitActiveCmdBufferForPresent(signalSemaphore);
    }
    else
    {
        // failed to acquire image from swapchain, do not present
        m_device->GetGfxQueue()->Submit(cmdBuffer);
        RecreateSwapchain();
        m_device->SubmitCommandsAndFlush();
        m_device->WaitForIdle();
        return true;
    }
    bool presentResult = m_swapchain->Present(m_renderingCompleteSemaphores[m_acquiredImageIndex]);
    if (RHIOptions::GetInstance().WaitForFrameCompletion())
    {
        WaitForFrameCompletion();
        IssueFrameEvent();
    }

    if (cmdBufferMgr->GetActiveCommandBufferDirect() &&
        !cmdBufferMgr->GetActiveCommandBufferDirect()->HasBegun())
    {
        cmdBufferMgr->SetupNewActiveCmdBuffer();
    }
    m_acquiredImageIndex = -1;
    m_presentCount++;

    return presentResult;
}

void VulkanViewport::Resize(uint32_t width, uint32_t height)
{
    const uint32_t oldWidth  = m_width;
    const uint32_t oldHeight = m_height;

    m_width  = width;
    m_height = height;
    RecreateSwapchain();
    if (!RHIOptions::GetInstance().UseDynamicRendering())
    {
        // create new one
        std::vector<TextureHandle> renderTargets;
        renderTargets.push_back(GetColorBackBuffer());
        renderTargets.push_back(GetDepthStencilBackBuffer());
        FramebufferInfo fbInfo{};
        fbInfo.width           = m_width;
        fbInfo.height          = m_height;
        fbInfo.numRenderTarget = 2;
        fbInfo.renderTargets   = renderTargets.data();

        for (auto& kv : m_framebufferCache)
        {
            VulkanFramebuffer* vkFramebuffer = kv.second;
            VkRenderPass renderPass          = vkFramebuffer->GetVkRenderPass();
            if (vkFramebuffer->GetWidth() == oldWidth && vkFramebuffer->GetHeight() == oldHeight)
            {
                // update old frame buffer
                vkDestroyFramebuffer(m_device->GetVkHandle(), vkFramebuffer->GetVkHandle(),
                                     nullptr);
                VulkanFramebuffer* newFramebuffer =
                    new VulkanFramebuffer(m_RHI, renderPass, fbInfo);
                m_framebufferCache[RenderPassHandle(renderPass)] = newFramebuffer;
            }
        }
    }
}

void VulkanViewport::Destroy()
{
    DestroySwapchain(nullptr);
    for (auto& semaphore : m_renderingCompleteSemaphores)
    {
        m_RHI->GetDevice()->GetSemaphoreManager()->ReleaseSemaphore(semaphore);
    }

    m_renderingCompleteSemaphores.clear();
    for (auto& kv : m_framebufferCache)
    {
        m_RHI->DestroyFramebuffer(FramebufferHandle(kv.second));
    }
}

FramebufferHandle VulkanViewport::GetCompatibleFramebufferForBackBuffer(
    RenderPassHandle renderPassHandle)
{
    VkRenderPass renderPass = TO_VK_RENDER_PASS(renderPassHandle);
    if ((m_framebufferCache.contains(renderPassHandle) &&
         m_framebufferCache[renderPassHandle]->GetVkRenderPass() != renderPass) ||
        !m_framebufferCache.contains(renderPassHandle))
    {
        std::vector<TextureHandle> renderTargets;
        renderTargets.push_back(GetColorBackBuffer());
        renderTargets.push_back(GetDepthStencilBackBuffer());
        FramebufferInfo fbInfo{};
        fbInfo.width           = m_width;
        fbInfo.height          = m_height;
        fbInfo.numRenderTarget = 2;
        fbInfo.renderTargets   = renderTargets.data();
        // save to cache
        m_framebufferCache[renderPassHandle] = new VulkanFramebuffer(m_RHI, renderPass, fbInfo);
    }
    return FramebufferHandle(m_framebufferCache[renderPassHandle]);
}

FramebufferHandle VulkanViewport::GetCompatibleFramebuffer(RenderPassHandle renderPassHandle,
                                                           const FramebufferInfo* fbInfo)
{
    VkRenderPass renderPass = TO_VK_RENDER_PASS(renderPassHandle);
    if ((m_framebufferCache.contains(renderPassHandle) &&
         m_framebufferCache[renderPassHandle]->GetVkRenderPass() != renderPass) ||
        !m_framebufferCache.contains(renderPassHandle))
    {
        // save to cache
        m_framebufferCache[renderPassHandle] = new VulkanFramebuffer(m_RHI, renderPass, *fbInfo);
    }
    return FramebufferHandle(m_framebufferCache[renderPassHandle]);
}
} // namespace zen::rhi