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
#include "Graphics/VulkanRHI/Platform/VulkanPlatformCommon.h"

namespace zen
{
namespace
{
struct ScopedViewportSurface
{
    VkInstance instance;
    VulkanSwapchainRecreateInfo info;

    ~ScopedViewportSurface()
    {
        // The swapchain clears the handle when it consumes ownership. Also covers
        // deferred zero-extent initialization and failures before that handoff.
        VulkanPlatform::DestroySurface(instance, info.surface);
    }
};

VkSurfaceKHR CreateViewportSurface(void* window, uint32_t width, uint32_t height)
{
    platform::GlfwWindowImpl* glfwWindow = static_cast<platform::GlfwWindowImpl*>(window);
    glfwWindow->CheckThreadOwnership();
    WindowData windowData{glfwWindow->GetHandle(), width, height};
    return VulkanPlatform::CreateSurface(GVulkanRHI->GetInstance(), &windowData);
}
} // namespace
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
    ScopedViewportSurface surface{GetInstance()};
    surface.info.surface = CreateViewportSurface(pWindow, width, height);
    return GetRHIThread().Invoke(&VulkanViewport::CreateObject, pWindow, width, height, enableVSync,
                                 &surface.info);
}

void VulkanRHI::DestroyViewport(RHIViewport* pViewport)
{
    pViewport->ReleaseReference();
}

VulkanViewport* VulkanViewport::CreateObject(void* pWindow,
                                             uint32_t width,
                                             uint32_t height,
                                             bool enableVSync,
                                             VulkanSwapchainRecreateInfo* surfaceInfo)
{
    ASSERT(GetRHIThread().IsCurrentThread());
    VulkanViewport* pViewport =
        VersatileResource::AllocMem<VulkanViewport>(GVulkanRHI->GetResourceAllocator());

    new (pViewport) VulkanViewport(pWindow, width, height, enableVSync);

    try
    {
        pViewport->Init();
        pViewport->CreateSwapchain(surfaceInfo);
    }
    catch (...)
    {
        pViewport->ReleaseReference();
        throw;
    }

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
}

void VulkanViewport::Destroy()
{
    DestroySwapchain(nullptr);

    if (m_framebuffer.vkHandle != VK_NULL_HANDLE)
    {
        vkDestroyFramebuffer(GVulkanRHI->GetVkDevice(), m_framebuffer.vkHandle, nullptr);
    }

    this->~VulkanViewport();

    VersatileResource::Free(GVulkanRHI->GetResourceAllocator(), this);
}

void VulkanViewport::CreateSwapchain(VulkanSwapchainRecreateInfo* pRecreateInfo)
{
    m_suspended = m_width == 0 || m_height == 0;
    if (m_suspended)
    {
        return;
    }

    m_pSwapchain = ZEN_NEW() VulkanSwapchain(m_width, m_height, m_enableVSync, pRecreateInfo);
    const VkImage* pImages   = m_pSwapchain->GetSwapchainImages();
    const uint32_t numImages = m_pSwapchain->GetNumSwapchainImages();
    if (numImages == 0)
    {
        m_suspended = true;
        return;
    }
    const VkExtent2D extent = m_pSwapchain->GetExtent();
    m_width                 = extent.width;
    m_height                = extent.height;
    m_swapchainImages.resize(numImages);
    FVulkanCommandListContext context(RHICommandContextType::eGraphics, m_pDevice);
    FVulkanCommandBuffer* pCmdBuffer = context.GetCommandBuffer();
    VkCommandBuffer cmdBuffer        = pCmdBuffer->GetVkHandle();

    // Swapchain images belong to the presentation engine until acquired. Their first
    // transition is recorded by PrepareForPresent after acquisition, with a semaphore
    // wait before CopyBackBufferToSwapchainImage overwrites the entire image.
    for (uint32_t i = 0; i < numImages; i++)
    {
        m_swapchainImages[i] = pImages[i];
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
    if (context.SubmitRecordedWorkloads() != RHISubmissionResult::eSuccess)
    {
        LOG_ERROR_AND_THROW("Failed to initialize viewport backbuffer layouts");
    }

    m_acquiredImageIndex = -1;
}

void VulkanViewport::DestroySwapchain(VulkanSwapchainRecreateInfo* pRecreateInfo)
{
    m_pDevice->WaitForIdle();

    if (m_pSwapchain != nullptr)
    {
        m_pSwapchain->Destroy(GVulkanRHI->AreSubmissionsBlocked() ? nullptr : pRecreateInfo);
        ZEN_DELETE(m_pSwapchain);
        m_pSwapchain = nullptr;
    }

    m_swapchainImages.clear();
    m_acquiredImageIndex      = -1;
    m_pImageAcquiredSemaphore = nullptr;
    m_presentSignalGeneration = 0;

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

bool VulkanViewport::BeginResize(uint32_t width,
                                 uint32_t height,
                                 VulkanSwapchainRecreateInfo* recreateInfo)
{
    ASSERT(GetRHIThread().IsCurrentThread());
    bool rebuild = false;
    if (GVulkanRHI->AreSubmissionsBlocked())
    {
        LOGE("Cannot resize while Vulkan submissions are blocked");
    }
    else if (width == 0 || height == 0)
    {
        // Preserve the last usable backbuffers while the window is minimized.
        m_suspended = true;
    }
    else
    {
        m_width  = width;
        m_height = height;
        const bool recreateSurface =
            m_pSwapchain != nullptr && m_pSwapchain->GetLastResult() == VK_ERROR_SURFACE_LOST_KHR;
        DestroySwapchain(recreateSurface ? nullptr : recreateInfo);
        if (GVulkanRHI->AreSubmissionsBlocked())
        {
            // Destruction is still legal after device loss, but recreation is not.
            if (recreateInfo->swapchain != VK_NULL_HANDLE)
            {
                vkDestroySwapchainKHR(m_pDevice->GetVkHandle(), recreateInfo->swapchain, nullptr);
            }
            VulkanPlatform::DestroySurface(GVulkanRHI->GetInstance(), recreateInfo->surface);
            *recreateInfo = {};
            LOGE("Cannot recreate a swapchain after a failed device idle wait");
        }
        else
        {
            rebuild = true;
        }
    }
    return rebuild;
}

void VulkanViewport::FinishResize(VulkanSwapchainRecreateInfo* recreateInfo)
{
    ASSERT(GetRHIThread().IsCurrentThread());
    CreateSwapchain(recreateInfo);
    VERIFY_EXPR(recreateInfo->surface == VK_NULL_HANDLE);
    VERIFY_EXPR(recreateInfo->swapchain == VK_NULL_HANDLE);
    if (m_framebuffer.vkHandle != VK_NULL_HANDLE)
    {
        vkDestroyFramebuffer(m_pDevice->GetVkHandle(), m_framebuffer.vkHandle, nullptr);
        m_framebuffer.vkHandle = VK_NULL_HANDLE;
    }
}

bool VulkanViewport::TryAcquireNextImage()
{
    if (m_suspended)
    {
        return false;
    }
    // A rejected presentation-copy submission did not consume the acquire semaphore.
    // Retry the already acquired image instead of leaking another swapchain acquisition.
    if (!m_suspended && m_acquiredImageIndex < 0 && m_pSwapchain != nullptr)
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
    GetRHIThread().CheckOwnership();
    if (GVulkanRHI->AreSubmissionsBlocked())
    {
        LOG_ERROR_AND_THROW("Cannot prepare presentation while Vulkan submissions are blocked");
    }
    if (TryAcquireNextImage())
    {
        m_presentAcquiredFailed = false;
        VulkanSemaphore* pRenderingCompleteSemaphore =
            m_pSwapchain->GetRenderingCompleteSemaphore(m_acquiredImageIndex);
        m_presentSignalGeneration = pRenderingCompleteSemaphore->GetSignalGeneration();

        FVulkanCommandListContext* pContext =
            static_cast<FVulkanCommandListContext*>(pCommandList->GetContext());

        const VkExtent2D extent = m_pSwapchain->GetExtent();

        pContext->AddWaitSemaphore(VK_PIPELINE_STAGE_TRANSFER_BIT, m_pImageAcquiredSemaphore);

        CopyBackBufferToSwapchainImage(pContext->GetCommandBuffer()->GetVkHandle(),
                                       m_swapchainImages[m_acquiredImageIndex], extent.width,
                                       extent.height);

        pContext->AddSignalSemaphore(pRenderingCompleteSemaphore);
    }
    else
    {
        m_presentAcquiredFailed = true;
    }
}

bool VulkanViewport::Present()
{
    GetRHIThread().CheckOwnership();
    bool result = false;
    if (GVulkanRHI->AreSubmissionsBlocked())
    {
        LOG_ERROR_AND_THROW("Cannot present while Vulkan submissions are blocked");
    }
    if (!m_suspended && m_pSwapchain != nullptr)
    {
        if (m_presentAcquiredFailed)
        {
            m_presentAcquiredFailed = false;
        }
        else if (m_acquiredImageIndex >= 0)
        {
            // Only a new accepted graphics-queue signal authorizes this copy.
            VulkanSemaphore* semaphore =
                m_pSwapchain->GetRenderingCompleteSemaphore(m_acquiredImageIndex);
            const uint64_t serial = semaphore->GetSignalSubmissionSerial(m_pDevice->GetGfxQueue(),
                                                                         m_presentSignalGeneration);
            if (serial != 0)
            {
                m_pSwapchain->MarkAcquireSemaphoreSubmitted(serial);
                result                    = m_pSwapchain->Present(semaphore);
                m_acquiredImageIndex      = -1;
                m_pImageAcquiredSemaphore = nullptr;
                m_presentSignalGeneration = 0;
                ++m_presentCount;
            }
        }
        if (m_pSwapchain->NeedsRecreation() && !GetRHIThread().IsThreaded())
        {
            Resize(m_width, m_height);
        }
    }
    return result;
}

bool VulkanViewport::NeedsRecreation() const
{
    return m_pSwapchain != nullptr && m_pSwapchain->NeedsRecreation();
}

void VulkanViewport::Resize(uint32_t width, uint32_t height)
{
    static_cast<platform::GlfwWindowImpl*>(m_pWindow)->CheckThreadOwnership();
    VulkanSwapchainRecreateInfo recreateInfo;
    if (GetRHIThread().Invoke(&VulkanViewport::BeginResize, this, width, height, &recreateInfo))
    {
        if (recreateInfo.surface == VK_NULL_HANDLE)
        {
            recreateInfo.surface = CreateViewportSurface(m_pWindow, width, height);
        }
        GetRHIThread().Invoke(&VulkanViewport::FinishResize, this, &recreateInfo);
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
