#pragma once
#include <vector>
#include "Graphics/VulkanRHI/VulkanHeaders.h"
#include "Common/SmallVector.h"

namespace zen::rhi
{
class VulkanRHI;
class VulkanDevice;
class VulkanSemaphore;
struct VulkanTexture;

struct VulkanSwapchainRecreateInfo
{
    VkSwapchainKHR swapchain;
    VkSurfaceKHR surface;
};

class VulkanSwapchain
{
public:
    VulkanSwapchain(VulkanRHI* RHI,
                    void* windowPtr,
                    uint32_t width,
                    uint32_t height,
                    bool enableVSync,
                    VulkanSwapchainRecreateInfo* recreateInfo);

    VkSwapchainKHR GetVkHandle() const
    {
        return m_swaphchain;
    }

    VkFormat GetFormat() const
    {
        return m_format;
    }

    auto& GetSwapchainImages() const
    {
        return m_swapchainImages;
    }

    int32_t AcquireNextImage(VulkanSemaphore** outSemaphore);

    bool Present(VulkanSemaphore* renderingCompleteSemaphore);

    void Destroy(VulkanSwapchainRecreateInfo* recreateInfo);

private:
    VulkanRHI* m_RHI{nullptr};
    VulkanDevice* m_device{nullptr};
    VkSwapchainKHR m_swaphchain{VK_NULL_HANDLE};
    VkSurfaceKHR m_surface{VK_NULL_HANDLE};
    uint32_t m_internalWidth{0};
    uint32_t m_internalHeight{0};
    VkFormat m_format{VK_FORMAT_UNDEFINED};
    VkColorSpaceKHR m_colorSpace{VK_COLORSPACE_SRGB_NONLINEAR_KHR};
    VkPresentModeKHR m_presentMode{VK_PRESENT_MODE_IMMEDIATE_KHR};
    SmallVector<VkImage> m_swapchainImages;
    int32_t m_imageIndex{-1};
    int32_t m_semaphoreIndex{0};
    // SmallVector<VulkanSemaphore*> m_imageAcquiredSemphores;
    std::vector<VulkanSemaphore*> m_imageAcquiredSemphores;

    friend class VulkanViewport;
};
} // namespace zen::rhi