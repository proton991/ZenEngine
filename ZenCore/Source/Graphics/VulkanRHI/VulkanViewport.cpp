#include "Graphics/VulkanRHI/VulkanViewport.h"
#include "Utils/Mutex.h"
#include "Graphics/RHI/RHIOptions.h"
#include "Graphics/VulkanRHI/VulkanTexture.h"
#include "Graphics/VulkanRHI/VulkanCommandBuffer.h"
#include "Graphics/VulkanRHI/VulkanCommands.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanQueue.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanRenderPass.h"
#include "Graphics/VulkanRHI/VulkanSynchronization.h"

namespace zen
{
// RHIViewport* RHIViewport::Create(void* pWindow, uint32_t width, uint32_t height, bool enableVSync)
// {
//     RHIViewport* pViewport = VulkanViewport::CreateObject(pWindow, width, height, enableVSync);
//
//     return pViewport;
//     //     static_cast<RHIViewport*>(ZEN_MEM_ALLOC_ZEROED(sizeof(VulkanViewport)));
//     //
//     // new (pViewport)
//     //     VulkanViewport(dynamic_cast<VulkanRHI*>(GDynamicRHI), pWindow, width, height, enableVSync);
//     //
//     // pViewport->Init();
//     //
//     // return pViewport;
// }

RHIViewport* VulkanRHI::CreateViewport(void* pWindow,
                                       uint32_t width,
                                       uint32_t height,
                                       bool enableVSync)
{
    RHIViewport* pViewport = VulkanViewport::CreateObject(pWindow, width, height, enableVSync);

    return pViewport;
}

void VulkanRHI::DestroyViewport(RHIViewport* viewport)
{
    viewport->ReleaseReference();
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

VulkanViewport* VulkanViewport::CreateObject(void* pWindow,
                                             uint32_t width,
                                             uint32_t height,
                                             bool enableVSync)
{
    VulkanViewport* pViewport = ZEN_NEW() VulkanViewport(pWindow, width, height, enableVSync);

    pViewport->Init();

    return pViewport;
}

VulkanViewport::VulkanViewport(void* windowPtr, uint32_t width, uint32_t height, bool enableVSync) :
    RHIViewport(windowPtr, width, height, enableVSync), m_device(GVulkanRHI->GetDevice())
// m_windowPtr(windowPtr),
// m_width(width),
// m_height(height),
// m_enableVSync(enableVSync)
{
    // m_depthFormat = m_RHI->GetSupportedDepthFormat();
    // LOGI("Viewport backbuffer depth format: {}", VkToString(static_cast<VkFormat>(m_depthFormat)));
    // CreateSwapchain(nullptr);
    // for (uint32_t i = 0; i < m_renderingCompleteSemaphores.size(); i++)
    // {
    //     auto* semaphore = m_RHI->GetDevice()->GetSemaphoreManager()->GetOrCreateSemaphore();
    //     const std::string debugName = "RenderComplete-" + std::to_string(i);
    //     semaphore->SetDebugName(debugName.c_str());
    //     m_renderingCompleteSemaphores[i] = semaphore;
    // }
}

void VulkanViewport::Init()
{
    m_depthFormat = GVulkanRHI->GetSupportedDepthFormat();
    LOGI("Viewport backbuffer depth format: {}", VkToString(static_cast<VkFormat>(m_depthFormat)));
    CreateSwapchain(nullptr);
    for (uint32_t i = 0; i < m_swapchain->GetNumSwapchainImages(); i++)
    {
        auto* semaphore = GVulkanRHI->GetDevice()->GetSemaphoreManager()->GetOrCreateSemaphore();
        const std::string debugName = "RenderComplete-" + std::to_string(i);
        semaphore->SetDebugName(debugName.c_str());
        m_renderingCompleteSemaphores[i] = semaphore;
    }
}

void VulkanViewport::Destroy()
{
    DestroySwapchain(nullptr);
    for (auto& semaphore : m_renderingCompleteSemaphores)
    {
        m_device->GetSemaphoreManager()->ReleaseSemaphore(semaphore);
    }

    if (m_framebuffer.vkHandle != VK_NULL_HANDLE)
    {
        vkDestroyFramebuffer(GVulkanRHI->GetVkDevice(), m_framebuffer.vkHandle, nullptr);
    }

    this->~VulkanViewport();

    ZEN_DELETE(this);
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
        m_device->GetGfxQueue()->GetLastSubmitInfo(m_lastFrameCmdBuffer,
                                                   &m_lastFenceSignaledCounter);
    }
}

void VulkanViewport::CreateSwapchain(VulkanSwapchainRecreateInfo* recreateInfo)
{
    m_colorBackBuffer        = nullptr;
    m_depthStencilBackBuffer = nullptr;

    m_swapchain =
        ZEN_NEW() VulkanSwapchain(m_pWindow, m_width, m_height, m_enableVSync, recreateInfo);
    const VkImage* images    = m_swapchain->GetSwapchainImages();
    const uint32_t numImages = m_swapchain->GetNumSwapchainImages();
    // m_renderingCompleteSemaphores.resize(numImages);
    // m_backBufferImages.resize(numImages);

    // VulkanCommandList* cmdList = dynamic_cast<VulkanCommandList*>(m_RHI->GetImmediateCommandList());
    VulkanCommandList* cmdList = m_device->GetImmediateCommandList();
    cmdList->BeginTransferWorkload();
    VulkanCommandBuffer* cmdBuffer = cmdList->GetCmdBufferManager()->GetUploadCommandBuffer();

    const VkImageSubresourceRange range =
        VulkanTexture::GetVkSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);
    VkClearColorValue clearColor{0.1f, 0.1f, 0.1f, 1.0f};
    for (uint32_t i = 0; i < numImages; i++)
    {
        m_backBufferImages[i] = images[i];
        cmdList->ChangeImageLayout(images[i], VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, range);
        vkCmdClearColorImage(cmdBuffer->GetVkHandle(), images[i],
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &range);
        cmdList->ChangeImageLayout(images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, range);
    }
    RHITextureCreateInfo colorTexInfo{};
    colorTexInfo.width  = m_width;
    colorTexInfo.height = m_height;
    colorTexInfo.format = GetSwapchainFormat();
    colorTexInfo.usageFlags.SetFlag(RHITextureUsageFlagBits::eColorAttachment);
    colorTexInfo.usageFlags.SetFlag(RHITextureUsageFlagBits::eTransferSrc);
    colorTexInfo.type = RHITextureType::e2D;
    colorTexInfo.tag  = "color_back_buffer";
    m_colorBackBuffer = VulkanTexture::CreateObject(colorTexInfo);

    RHITextureCreateInfo depthStencilTexInfo{};
    depthStencilTexInfo.width  = m_width;
    depthStencilTexInfo.height = m_height;
    depthStencilTexInfo.format = GetDepthStencilFormat();
    depthStencilTexInfo.usageFlags.SetFlag(RHITextureUsageFlagBits::eDepthStencilAttachment);
    depthStencilTexInfo.type = RHITextureType::e2D;
    depthStencilTexInfo.tag  = "depth_stencil_back_buffer";
    m_depthStencilBackBuffer = VulkanTexture::CreateObject(depthStencilTexInfo);
    // add image barriers, transfer back buffer layout
    VulkanPipelineBarrier barrier;
    barrier.AddImageBarrier(m_colorBackBuffer->GetVkImage(), VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            m_colorBackBuffer->GetVkSubresourceRange());
    barrier.AddImageBarrier(m_depthStencilBackBuffer->GetVkImage(), VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                            m_depthStencilBackBuffer->GetVkSubresourceRange());
    barrier.Execute(cmdBuffer);
    cmdList->EndTransferWorkload();

    m_acquiredImageIndex = -1;
}

void VulkanViewport::DestroySwapchain(VulkanSwapchainRecreateInfo* recreateInfo)
{
    m_device->WaitForIdle();
    if (m_swapchain != nullptr)
    {
        for (uint32_t i = 0; i < m_swapchain->GetNumSwapchainImages(); i++)
        {
            m_backBufferImages[i] = VK_NULL_HANDLE;
        }
        m_swapchain->Destroy(recreateInfo);
        ZEN_DELETE(m_swapchain);
        m_swapchain = nullptr;
    }
    if (m_colorBackBuffer)
    {
        GVulkanRHI->DestroyTexture(m_colorBackBuffer);
        m_colorBackBuffer = nullptr;
    }
    if (m_depthStencilBackBuffer)
    {
        GVulkanRHI->DestroyTexture(m_depthStencilBackBuffer);
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
        barrier.AddImageBarrier(m_colorBackBuffer->GetVkImage(), prevLayout,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                m_colorBackBuffer->GetVkSubresourceRange());
        barrier.AddImageBarrier(
            dstImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VulkanTexture::GetVkSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1));
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
        vkCmdBlitImage(cmdBuffer->GetVkHandle(), m_colorBackBuffer->GetVkImage(),
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
        vkCmdCopyImage(cmdBuffer->GetVkHandle(), m_colorBackBuffer->GetVkImage(),
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImage,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
    {
        VulkanPipelineBarrier barrier;
        barrier.AddImageBarrier(m_colorBackBuffer->GetVkImage(),
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, prevLayout,
                                m_colorBackBuffer->GetVkSubresourceRange());
        barrier.AddImageBarrier(
            dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VulkanTexture::GetVkSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1));
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
    if (m_framebuffer.vkHandle != VK_NULL_HANDLE)
    {
        vkDestroyFramebuffer(m_device->GetVkHandle(), m_framebuffer.vkHandle, nullptr);
        m_framebuffer.vkHandle = VK_NULL_HANDLE;
    }
}

VkFramebuffer VulkanViewport::GetCompatibleFramebufferForBackBuffer(VkRenderPass renderPass)
{

    if (m_framebuffer.vkHandle == VK_NULL_HANDLE)
    {
        const uint32_t numAttachments = 2;
        VkImageView imageViews[numAttachments];

        imageViews[0] = m_colorBackBuffer->GetVkImageView();
        imageViews[1] = m_depthStencilBackBuffer->GetVkImageView();

        VkFramebufferCreateInfo framebufferCI;
        InitVkStruct(framebufferCI, VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
        framebufferCI.renderPass      = renderPass;
        framebufferCI.width           = m_width;
        framebufferCI.height          = m_height;
        framebufferCI.layers          = 1;
        framebufferCI.attachmentCount = numAttachments;
        framebufferCI.pAttachments    = imageViews;
        VKCHECK(vkCreateFramebuffer(GVulkanRHI->GetVkDevice(), &framebufferCI, nullptr,
                                    &m_framebuffer.vkHandle));
        m_framebuffer.vkRenderPass = renderPass;
    }
    return m_framebuffer.vkHandle;
}

// FramebufferHandle VulkanViewport::GetCompatibleFramebuffer(RenderPassHandle renderPassHandle,
//                                                            const RHIFramebufferInfo* fbInfo)
// {
//     VkRenderPass renderPass = TO_VK_RENDER_PASS(renderPassHandle);
//     if ((m_framebufferCache.contains(renderPassHandle) &&
//          m_framebufferCache[renderPassHandle]->GetVkRenderPass() != renderPass) ||
//         !m_framebufferCache.contains(renderPassHandle))
//     {
//         // save to cache
//         m_framebufferCache[renderPassHandle] =
//             new VulkanFramebuffer(GVulkanRHI, renderPass, *fbInfo);
//     }
//     return FramebufferHandle(m_framebufferCache[renderPassHandle]);
// }

RHITexture* VulkanViewport::GetColorBackBuffer()
{
    return m_colorBackBuffer;
}

RHITexture* VulkanViewport::GetDepthStencilBackBuffer()
{
    return m_depthStencilBackBuffer;
}
} // namespace zen