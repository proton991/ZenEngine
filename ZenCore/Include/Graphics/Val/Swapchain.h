#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include "DeviceObject.h"

namespace zen::val
{
class Swapchain : public DeviceObject<VkSwapchainKHR, VK_OBJECT_TYPE_SWAPCHAIN_KHR>
{
public:
    Swapchain(const Device& device,
              VkSurfaceKHR surface,
              VkExtent2D extent,
              VkSwapchainKHR oldSwapchain = VK_NULL_HANDLE,
              uint32_t imageCount         = 3,
              bool vsync                  = true);

    ~Swapchain() = default;

    VkFormat GetFormat() const
    {
        return m_surfaceFormat.format;
    }

    VkImageUsageFlags GetUsage() const
    {
        return m_usage;
    }

    auto& GetImages() const
    {
        return m_images;
    }

    VkExtent3D GetExtent3D() const
    {
        return {m_extent.width, m_extent.height, 1};
    }

    VkExtent2D GetExtent2D() const
    {
        return m_extent;
    }

    VkResult AcquireNextImage(uint32_t& imageIndex,
                              VkSemaphore imageAcquiredSem,
                              VkFence fence = VK_NULL_HANDLE);

private:
    VkImageUsageFlags ChooseImageUsage(VkImageUsageFlags supportedUsage);
    VkPresentModeKHR ChoosePresentMode(bool vsync);
    VkCompositeAlphaFlagBitsKHR ChooseCompositeAlpha(VkCompositeAlphaFlagBitsKHR request,
                                                     VkCompositeAlphaFlagsKHR supported);
    VkSurfaceFormatKHR ChooseSurfaceFormat();

    VkSurfaceKHR m_surface{VK_NULL_HANDLE};
    std::vector<VkSurfaceFormatKHR> m_surfaceFormats{};
    std::vector<VkPresentModeKHR> m_presentModes{};
    VkSurfaceFormatKHR m_surfaceFormat;
    VkExtent2D m_extent;
    VkImageUsageFlags m_usage;
    std::vector<VkImage> m_images;
    bool m_vsync;
};
} // namespace zen::val