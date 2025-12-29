#pragma once
#include "Templates/HashMap.h"
// #include "Templates/SmallVector.h"
#include "Graphics/VulkanRHI/VulkanHeaders.h"
#include "Graphics/VulkanRHI/VulkanSwapchain.h"
#include "Graphics/RHI/RHIResource.h"
// #define NUM_FRAMES 3

namespace zen
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
    static VulkanViewport* CreateObject(void* pWindow,
                                        uint32_t width,
                                        uint32_t height,
                                        bool enableVSync);

    ~VulkanViewport() {}

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

    RHITexture* GetColorBackBuffer() final;

    RHITextureSubResourceRange GetColorBackBufferRange() final
    {
        RHITextureSubResourceRange range;
        range.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
        range.layerCount     = 1;
        range.levelCount     = 1;
        range.baseMipLevel   = 0;
        range.baseArrayLayer = 0;
        return range;
    }

    RHITexture* GetDepthStencilBackBuffer() final;

    RHITextureSubResourceRange GetDepthStencilBackBufferRange() final
    {
        RHITextureSubResourceRange range{};
        range.aspect.SetFlag(RHITextureAspectFlagBits::eDepth);
        range.aspect.SetFlag(RHITextureAspectFlagBits::eStencil);
        range.layerCount     = 1;
        range.levelCount     = 1;
        range.baseMipLevel   = 0;
        range.baseArrayLayer = 0;
        return range;
    }

    // FramebufferHandle GetCompatibleFramebuffer(RenderPassHandle renderPassHandle,
    //                                            const RHIFramebufferInfo* fbInfo) final;

    VkFramebuffer GetCompatibleFramebufferForBackBuffer(VkRenderPass renderPass);

    void Resize(uint32_t width, uint32_t height) final;

protected:
    void Init() override;

    void Destroy() override;

private:
    VulkanViewport(void* windowPtr, uint32_t width, uint32_t height, bool enableVSync);

    void CreateSwapchain(VulkanSwapchainRecreateInfo* recreateInfo);
    void DestroySwapchain(VulkanSwapchainRecreateInfo* recreateInfo);
    bool TryAcquireNextImage();
    void RecreateSwapchain();
    void CopyToBackBufferForPresent(VulkanCommandBuffer* cmdBuffer,
                                    VkImage dstImage,
                                    uint32_t windowWidth,
                                    uint32_t windowHeight);

    // VulkanRHI* m_RHI{nullptr};
    VulkanDevice* m_device{nullptr};
    // void* m_windowPtr{nullptr};
    // uint32_t m_width{0};
    // uint32_t m_height{0};
    // bool m_enableVSync{true};
    DataFormat m_depthFormat;
    VulkanSwapchain* m_swapchain{nullptr};
    int32_t m_acquiredImageIndex{-1};
    VulkanSemaphore* m_imageAcquiredSemaphore{nullptr};
    VulkanSemaphore* m_renderingCompleteSemaphores[ZEN_NUM_FRAMES_IN_FLIGHT];
    VulkanCommandBuffer* m_lastFrameCmdBuffer{nullptr};
    uint64_t m_lastFenceSignaledCounter{0};
    // SmallVector<VkImage, NUM_FRAMES> m_backBufferImages;
    VkImage m_backBufferImages[ZEN_NUM_FRAMES_IN_FLIGHT];
    VulkanTexture* m_colorBackBuffer{nullptr};
    VulkanTexture* m_depthStencilBackBuffer{nullptr};

    struct
    {
        VkFramebuffer vkHandle{VK_NULL_HANDLE};
        VkRenderPass vkRenderPass{VK_NULL_HANDLE};
    } m_framebuffer;

    // HashMap<RenderPassHandle, VulkanFramebuffer*> m_framebufferCache;
    uint64_t m_presentCount{0};
};
} // namespace zen