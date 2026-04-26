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
struct FVulkanCommandListContext;

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
        return static_cast<DataFormat>(m_pSwapchain->GetFormat());
    }

    DataFormat GetDepthStencilFormat() final
    {
        return m_depthFormat;
    }

    bool Present(VulkanCommandBuffer* pCmdBuffer);

    bool Present(FVulkanCommandListContext* pContext);

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
    VulkanViewport(void* pWindowPtr, uint32_t width, uint32_t height, bool enableVSync);

    void CreateSwapchain(VulkanSwapchainRecreateInfo* pRecreateInfo);
    void DestroySwapchain(VulkanSwapchainRecreateInfo* pRecreateInfo);
    bool TryAcquireNextImage();
    void RecreateSwapchain();
    void CopyToBackBufferForPresent(VkCommandBuffer cmdBufferVk,
                                    VkImage dstImage,
                                    uint32_t windowWidth,
                                    uint32_t windowHeight);

    // VulkanRHI* m_RHI{nullptr};
    VulkanDevice* m_pDevice{nullptr};
    // void* m_windowPtr{nullptr};
    // uint32_t m_width{0};
    // uint32_t m_height{0};
    // bool m_enableVSync{true};
    DataFormat m_depthFormat;
    VulkanSwapchain* m_pSwapchain{nullptr};
    int32_t m_acquiredImageIndex{-1};
    VulkanSemaphore* m_pImageAcquiredSemaphore{nullptr};
    VulkanSemaphore* m_pRenderingCompleteSemaphores[ZEN_NUM_FRAMES_IN_FLIGHT];
    VulkanCommandBuffer* m_pLastFrameCmdBuffer{nullptr};
    uint64_t m_lastFenceSignaledCounter{0};
    VkImage m_swapchainImages[ZEN_NUM_FRAMES_IN_FLIGHT];
    VulkanTexture* m_pColorBackBuffer{nullptr};
    VulkanTexture* m_pDepthStencilBackBuffer{nullptr};

    struct
    {
        VkFramebuffer vkHandle{VK_NULL_HANDLE};
        VkRenderPass vkRenderPass{VK_NULL_HANDLE};
    } m_framebuffer;

    // HashMap<RenderPassHandle, VulkanFramebuffer*> m_framebufferCache;
    uint64_t m_presentCount{0};
};
} // namespace zen
