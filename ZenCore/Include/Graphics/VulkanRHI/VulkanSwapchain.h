#pragma once

#include "Graphics/VulkanRHI/VulkanHeaders.h"
#include "Common/SmallVector.h"

namespace zen::rhi
{
class VulkanRHI;
struct VulkanSurface;

class VulkanSwapchain
{
public:
    VulkanSwapchain(VulkanRHI* vkRHI,
                    VulkanSurface* vulkanSurface,
                    bool enableVSync,
                    VkSwapchainKHR oldSwapchain = VK_NULL_HANDLE);

    VkSwapchainKHR GetVkHandle() const { return m_swaphchain; }

private:
    VkSwapchainKHR m_swaphchain{VK_NULL_HANDLE};
    VkFormat m_format{VK_FORMAT_UNDEFINED};
    VkColorSpaceKHR m_colorSpace{VK_COLORSPACE_SRGB_NONLINEAR_KHR};
    VkPresentModeKHR m_presentMode{VK_PRESENT_MODE_IMMEDIATE_KHR};
    SmallVector<VkImage> m_swapchainImages;
};
} // namespace zen::rhi