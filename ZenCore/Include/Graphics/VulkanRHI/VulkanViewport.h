#pragma once
#include "Graphics/RHI/RHICommandList.h"
#include "Templates/HeapVector.h"
#include "Graphics/VulkanRHI/VulkanHeaders.h"
#include "Graphics/VulkanRHI/VulkanSwapchain.h"
#include "Graphics/RHI/RHIResource.h"
namespace zen
{
class VulkanRHI;
class VulkanQueue;
class VulkanDevice;
class VulkanSemaphore;
class VulkanCommandBuffer;
struct VulkanTexture;
struct FVulkanCommandListContext;

class VulkanViewport : public RHIViewport
{
public:
    static VulkanViewport* CreateObject(void*                        pWindow,
                                        uint32_t                     width,
                                        uint32_t                     height,
                                        bool                         enableVSync,
                                        VulkanSwapchainRecreateInfo* surfaceInfo);

    ~VulkanViewport() {}

    uint32_t GetWidth() const final
    {
        return m_width;
    }

    uint32_t GetHeight() const final
    {
        return m_height;
    }

    DataFormat GetSwapchainFormat() final
    {
        return m_pSwapchain != nullptr ? static_cast<DataFormat>(m_pSwapchain->GetBackBufferFormat()) : DataFormat::eUndefined;
    }

    DataFormat GetDepthStencilFormat() final
    {
        return m_depthFormat;
    }

    void PrepareForPresent(RHICommandList* pCmdList) final;

    bool Present() final;

    RHIStatus PrepareForPresentChecked(RHICommandList* commands) override;

    RHIPresentResult PresentChecked() override;

    bool NeedsRecreation() const final;

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

        if (FormatIsDepthStencil(m_depthFormat))
        {
            range.aspect.SetFlag(RHITextureAspectFlagBits::eStencil);
        }

        range.layerCount     = 1;
        range.levelCount     = 1;
        range.baseMipLevel   = 0;
        range.baseArrayLayer = 0;

        return range;
    }

    void Resize(uint32_t width, uint32_t height) final;

protected:
    void Init() override;

    void Destroy() override;

private:
    VulkanViewport(void* pWindowPtr, uint32_t width, uint32_t height, bool enableVSync);

    bool CreateSwapchain(VulkanSwapchainRecreateInfo* pRecreateInfo);

    void DestroySwapchain(VulkanSwapchainRecreateInfo* pRecreateInfo);

    bool TryAcquireNextImage();

    bool PresentInternal(bool deferRecreation);

    bool BeginResize(uint32_t width, uint32_t height, VulkanSwapchainRecreateInfo* recreateInfo);

    void FinishResize(VulkanSwapchainRecreateInfo* recreateInfo);

    void CopyBackBufferToSwapchainImage(VkCommandBuffer cmdBufferVk,
                                        VkImage         dstImage,
                                        uint32_t        windowWidth,
                                        uint32_t        windowHeight);

    VulkanDevice* m_pDevice{nullptr};

    DataFormat          m_depthFormat;
    VulkanSwapchain*    m_pSwapchain{nullptr};
    int32_t             m_acquiredImageIndex{-1};
    VulkanSemaphore*    m_pImageAcquiredSemaphore{nullptr};
    HeapVector<VkImage> m_swapchainImages;
    VulkanTexture*      m_pColorBackBuffer{nullptr};
    VulkanTexture*      m_pDepthStencilBackBuffer{nullptr};
    uint64_t            m_presentCount{0};

    uint64_t         m_presentSignalGeneration{0};
    bool             m_presentAcquiredFailed{false};
    bool             m_suspended{false};
    RHIPresentResult m_presentResult{};
};
} // namespace zen
