#pragma once
#include "Common/HashMap.h"
#include "Common/SmallVector.h"
#include "Graphics/VulkanRHI/VulkanHeaders.h"
#include "Graphics/VulkanRHI/VulkanSwapchain.h"
#include "Graphics/RHI/RHIResource.h"
#define NUM_FRAMES 3

namespace zen::rhi
{
class VulkanRHI;
class VulkanQueue;
class VulkanDevice;
class VulkanSemaphore;
class VulkanCommandBuffer;
class VulkanFramebuffer;
struct VulkanTexture;

class VulkanViewport : public RHIViewport
{
public:
    VulkanViewport(VulkanRHI* RHI,
                   void* windowPtr,
                   uint32_t width,
                   uint32_t height,
                   bool enableVSync);

    void Destroy();

    uint32_t GetWidth() const final
    {
        return m_width;
    }

    uint32_t GetHeight() const final
    {
        return m_height;
    }

    void WaitForFrameCompletion() final;

    void IssueFrameEvent() final;

    DataFormat GetSwapchainFormat() final
    {
        return static_cast<DataFormat>(m_swapchain->GetFormat());
    }

    DataFormat GetDepthStencilFormat() final
    {
        return m_depthFormat;
    }

    bool Present(VulkanCommandBuffer* cmdBuffer);

    TextureHandle GetColorBackBuffer() final
    {
        return TextureHandle(m_colorBackBuffer);
    }

    TextureHandle GetDepthStencilBackBuffer() final
    {
        return TextureHandle(m_depthStencilBackBuffer);
    }

    FramebufferHandle GetCompatibleFramebuffer(RenderPassHandle renderPassHandle,
                                               const FramebufferInfo* fbInfo) final;

    FramebufferHandle GetCompatibleFramebufferForBackBuffer(
        RenderPassHandle renderPassHandle) final;

    void Resize(uint32_t width, uint32_t height) final;

private:
    void CreateSwapchain(VulkanSwapchainRecreateInfo* recreateInfo);
    void DestroySwapchain(VulkanSwapchainRecreateInfo* recreateInfo);
    bool TryAcquireNextImage();
    void RecreateSwapchain();
    void CopyToBackBufferForPresent(VulkanCommandBuffer* cmdBuffer,
                                    VkImage dstImage,
                                    uint32_t windowWidth,
                                    uint32_t windowHeight);

    VulkanRHI* m_RHI{nullptr};
    VulkanDevice* m_device{nullptr};
    void* m_windowPtr{nullptr};
    uint32_t m_width{0};
    uint32_t m_height{0};
    bool m_enableVSync{true};
    DataFormat m_depthFormat;
    VulkanSwapchain* m_swapchain{nullptr};
    int32_t m_acquiredImageIndex{-1};
    VulkanSemaphore* m_imageAcquiredSemaphore{nullptr};
    std::vector<VulkanSemaphore*> m_renderingCompleteSemaphores;
    VulkanCommandBuffer* m_lastFrameCmdBuffer{nullptr};
    uint64_t m_lastFenceSignaledCounter{0};
    SmallVector<VkImage, NUM_FRAMES> m_backBufferImages;
    VulkanTexture* m_colorBackBuffer{nullptr};
    VulkanTexture* m_depthStencilBackBuffer{nullptr};
    HashMap<RenderPassHandle, VulkanFramebuffer*> m_framebufferCache;
    uint64_t m_presentCount{0};
};
} // namespace zen::rhi