#include "Graphics/VulkanRHI/VulkanViewport.h"
#include "Graphics/RHI/RHICommandList.h"
#include "Utils/Mutex.h"
#include "Graphics/RHI/RHIOptions.h"
#include "Graphics/VulkanRHI/VulkanTexture.h"
#include "Graphics/VulkanRHI/VulkanCommandList.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanBuffer.h"
#include "Graphics/VulkanRHI/VulkanDescriptorPool.h"
#include "Graphics/VulkanRHI/VulkanQueue.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanRenderPass.h"
#include "Graphics/VulkanRHI/VulkanResourceAllocator.h"
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

void VulkanRHI::DestroyViewport(RHIViewport* pViewport)
{
    pViewport->ReleaseReference();
}

VulkanViewport* VulkanViewport::CreateObject(void* pWindow,
                                             uint32_t width,
                                             uint32_t height,
                                             bool enableVSync)
{
    VulkanViewport* pViewport =
        VersatileResource::AllocMem<VulkanViewport>(GVulkanRHI->GetResourceAllocator());

    new (pViewport) VulkanViewport(pWindow, width, height, enableVSync);

    pViewport->Init();

    return pViewport;
}

VulkanViewport::VulkanViewport(void* pWindowPtr,
                               uint32_t width,
                               uint32_t height,
                               bool enableVSync) :
    RHIViewport(pWindowPtr, width, height, enableVSync), m_pDevice(GVulkanRHI->GetDevice())
// m_windowPtr(windowPtr),
// m_width(width),
// m_height(height),
// m_enableVSync(enableVSync)
{}

void VulkanViewport::Init()
{
    m_depthFormat = GVulkanRHI->GetSupportedDepthFormat();
    LOGI("Viewport backbuffer depth format: {}", VkToString(static_cast<VkFormat>(m_depthFormat)));
    CreateSwapchain(nullptr);

    for (uint32_t i = 0; i < m_pSwapchain->GetNumSwapchainImages(); i++)
    {
        VulkanSemaphore* pSemaphore =
            GVulkanRHI->GetDevice()->GetSemaphoreManager()->GetOrCreateSemaphore();
        const NameID debugName(fmt::format("RenderComplete-{}", i));
        pSemaphore->SetDebugName(debugName);
        m_pRenderingCompleteSemaphores[i] = pSemaphore;
    }
}

void VulkanViewport::Destroy()
{
    DestroySwapchain(nullptr);

    for (VulkanSemaphore*& semaphore : m_pRenderingCompleteSemaphores)
    {
        m_pDevice->GetSemaphoreManager()->ReleaseSemaphore(semaphore);
    }

    if (m_framebuffer.vkHandle != VK_NULL_HANDLE)
    {
        vkDestroyFramebuffer(GVulkanRHI->GetVkDevice(), m_framebuffer.vkHandle, nullptr);
    }

    this->~VulkanViewport();

    VersatileResource::Free(GVulkanRHI->GetResourceAllocator(), this);
}

void VulkanViewport::CreateSwapchain(VulkanSwapchainRecreateInfo* pRecreateInfo)
{
    m_pColorBackBuffer        = nullptr;
    m_pDepthStencilBackBuffer = nullptr;

    m_pSwapchain =
        ZEN_NEW() VulkanSwapchain(m_pWindow, m_width, m_height, m_enableVSync, pRecreateInfo);
    const VkImage* pImages   = m_pSwapchain->GetSwapchainImages();
    const uint32_t numImages = m_pSwapchain->GetNumSwapchainImages();
    // m_renderingCompleteSemaphores.resize(numImages);
    // m_swapchainImages.resize(numImages);

    FVulkanCommandListContext context(RHICommandContextType::eGraphics, m_pDevice);
    FVulkanCommandBuffer* pCmdBuffer = context.GetCommandBuffer();
    VkCommandBuffer cmdBuffer        = pCmdBuffer->GetVkHandle();

    const VkImageSubresourceRange range =
        VulkanTexture::GetVkSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);
    VkClearColorValue clearColor{0.1f, 0.1f, 0.1f, 1.0f};

    for (uint32_t i = 0; i < numImages; i++)
    {
        m_swapchainImages[i] = pImages[i];

        {
            VulkanPipelineBarrier barrier;
            barrier.AddImageBarrier(pImages[i], VK_IMAGE_LAYOUT_UNDEFINED,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, range);
            barrier.ExecuteImageBarriersOnly(cmdBuffer);
        }

        vkCmdClearColorImage(cmdBuffer, pImages[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &clearColor, 1, &range);

        {
            VulkanPipelineBarrier barrier;
            barrier.AddImageBarrier(pImages[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, range);
            barrier.ExecuteImageBarriersOnly(cmdBuffer);
        }
    }

    RHITextureCreateInfo colorTexInfo{};
    colorTexInfo.width  = m_width;
    colorTexInfo.height = m_height;
    colorTexInfo.format = GetSwapchainFormat();
    colorTexInfo.usageFlags.SetFlag(RHITextureUsageFlagBits::eColorAttachment);
    colorTexInfo.usageFlags.SetFlag(RHITextureUsageFlagBits::eTransferSrc);
    colorTexInfo.type  = RHITextureType::e2D;
    colorTexInfo.tag   = "color_back_buffer";
    m_pColorBackBuffer = VulkanTexture::CreateObject(colorTexInfo);

    RHITextureCreateInfo depthStencilTexInfo{};
    depthStencilTexInfo.width  = m_width;
    depthStencilTexInfo.height = m_height;
    depthStencilTexInfo.format = GetDepthStencilFormat();
    depthStencilTexInfo.usageFlags.SetFlag(RHITextureUsageFlagBits::eDepthStencilAttachment);
    depthStencilTexInfo.type  = RHITextureType::e2D;
    depthStencilTexInfo.tag   = "depth_stencil_back_buffer";
    m_pDepthStencilBackBuffer = VulkanTexture::CreateObject(depthStencilTexInfo);
    // add image barriers, transfer back buffer layout
    VulkanPipelineBarrier barrier;
    barrier.AddImageBarrier(m_pColorBackBuffer->GetVkImage(), VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            m_pColorBackBuffer->GetVkSubresourceRange());
    barrier.AddImageBarrier(m_pDepthStencilBackBuffer->GetVkImage(), VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                            m_pDepthStencilBackBuffer->GetVkSubresourceRange());
    barrier.ExecuteImageBarriersOnly(cmdBuffer);
    context.SubmitRecordedWorkloads();
    // m_device->WaitForIdle();

    m_acquiredImageIndex = -1;
}

void VulkanViewport::DestroySwapchain(VulkanSwapchainRecreateInfo* pRecreateInfo)
{
    m_pDevice->WaitForIdle();

    if (m_pSwapchain != nullptr)
    {
        for (uint32_t i = 0; i < m_pSwapchain->GetNumSwapchainImages(); i++)
        {
            m_swapchainImages[i] = VK_NULL_HANDLE;
        }

        m_pSwapchain->Destroy(pRecreateInfo);
        ZEN_DELETE(m_pSwapchain);
        m_pSwapchain = nullptr;
    }

    if (m_pColorBackBuffer)
    {
        GVulkanRHI->DestroyTexture(m_pColorBackBuffer);
        m_pColorBackBuffer = nullptr;
    }

    if (m_pDepthStencilBackBuffer)
    {
        GVulkanRHI->DestroyTexture(m_pDepthStencilBackBuffer);
        m_pDepthStencilBackBuffer = nullptr;
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
    // A rejected presentation-copy submission did not consume the acquire semaphore.
    // Retry the already acquired image instead of leaking another swapchain acquisition.
    if (m_acquiredImageIndex < 0 && m_pSwapchain != nullptr)
    {
        const int32_t imageIndex = m_pSwapchain->AcquireNextImage(&m_pImageAcquiredSemaphore);

        if (imageIndex >= 0)
        {
            m_acquiredImageIndex = imageIndex;
        }
    }

    return m_acquiredImageIndex >= 0;
}

void VulkanViewport::CopyBackBufferToSwapchainImage(VkCommandBuffer cmdBufferVk,
                                                    VkImage dstImage,
                                                    uint32_t windowWidth,
                                                    uint32_t windowHeight)
{
    const VkImageLayout prevLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    {
        VulkanPipelineBarrier barrier;
        barrier.AddImageBarrier(m_pColorBackBuffer->GetVkImage(), prevLayout,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                m_pColorBackBuffer->GetVkSubresourceRange());
        barrier.AddImageBarrier(
            dstImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VulkanTexture::GetVkSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1));
        // TRANSFER chains the acquired-image transition after PrepareForPresent's semaphore
        // wait. COLOR_ATTACHMENT_OUTPUT also covers the backbuffer writes in this batch.
        barrier.Execute(cmdBufferVk,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT);
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
        vkCmdBlitImage(cmdBufferVk, m_pColorBackBuffer->GetVkImage(),
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
        vkCmdCopyImage(cmdBufferVk, m_pColorBackBuffer->GetVkImage(),
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImage,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }

    {
        VulkanPipelineBarrier barrier;
        barrier.AddImageBarrier(m_pColorBackBuffer->GetVkImage(),
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, prevLayout,
                                m_pColorBackBuffer->GetVkSubresourceRange());
        barrier.AddImageBarrier(
            dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VulkanTexture::GetVkSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1));
        barrier.ExecuteImageBarriersOnly(cmdBufferVk);
    }
}

void VulkanViewport::PrepareForPresent(RHICommandList* pCommandList)
{
    if (TryAcquireNextImage())
    {
        m_presentAcquiredFailed = false;

        m_pContext = static_cast<FVulkanCommandListContext*>(pCommandList->GetContext());

        uint32_t windowWidth  = std::min(m_width, m_pSwapchain->m_internalWidth);
        uint32_t windowHeight = std::min(m_height, m_pSwapchain->m_internalHeight);

        m_pContext->AddWaitSemaphore(VK_PIPELINE_STAGE_TRANSFER_BIT, m_pImageAcquiredSemaphore);

        CopyBackBufferToSwapchainImage(m_pContext->GetCommandBuffer()->GetVkHandle(),
                                       m_swapchainImages[m_acquiredImageIndex], windowWidth,
                                       windowHeight);

        m_pContext->AddSignalSemaphore(m_pRenderingCompleteSemaphores[m_acquiredImageIndex]);
    }
    else
    {
        m_presentAcquiredFailed = true;
    }
}

bool VulkanViewport::Present()
{
    bool presentResult;

    if (m_presentAcquiredFailed)
    {
        m_presentAcquiredFailed = false;
        RecreateSwapchain();
        presentResult = false;
    }
    else
    {
        m_pSwapchain->MarkAcquireSemaphoreSubmitted(m_pContext->GetLastSubmittedSerial());

        presentResult = m_pSwapchain->Present(m_pRenderingCompleteSemaphores[m_acquiredImageIndex]);
    }

    m_acquiredImageIndex      = -1;
    m_pImageAcquiredSemaphore = nullptr;
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
        vkDestroyFramebuffer(m_pDevice->GetVkHandle(), m_framebuffer.vkHandle, nullptr);
        m_framebuffer.vkHandle = VK_NULL_HANDLE;
    }
}

VkFramebuffer VulkanViewport::GetCompatibleFramebufferForBackBuffer(VkRenderPass renderPass)
{
    if (m_framebuffer.vkHandle == VK_NULL_HANDLE)
    {
        const uint32_t numAttachments = 2;
        VkImageView imageViews[numAttachments];

        imageViews[0] = m_pColorBackBuffer->GetVkImageView();
        imageViews[1] = m_pDepthStencilBackBuffer->GetVkImageView();

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
    return m_pColorBackBuffer;
}

RHITexture* VulkanViewport::GetDepthStencilBackBuffer()
{
    return m_pDepthStencilBackBuffer;
}
} // namespace zen
