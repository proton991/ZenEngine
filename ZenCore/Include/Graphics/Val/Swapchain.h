#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include "DeviceObject.h"

namespace zen::val
{
class Swapchain : public DeviceObject<VkSwapchainKHR, VK_OBJECT_TYPE_SWAPCHAIN_KHR>
{
public:
    Swapchain(Device& device, VkSurfaceKHR surface, VkExtent2D extent, VkSwapchainKHR oldSwapchain = VK_NULL_HANDLE, uint32_t imageCount = 3, bool vsync = true);

    ~Swapchain();

private:
    VkPresentModeKHR            ChoosePresentMode(bool vsync);
    VkCompositeAlphaFlagBitsKHR ChooseCompositeAlpha(VkCompositeAlphaFlagBitsKHR request, VkCompositeAlphaFlagsKHR supported);

    VkSurfaceKHR                    m_surface{VK_NULL_HANDLE};
    std::vector<VkSurfaceFormatKHR> m_surfaceFormats{};
    std::vector<VkPresentModeKHR>   m_presentModes{};
    VkFormat                        m_format;
    VkExtent2D                      m_extent;
    VkImageUsageFlags               m_usage;
    std::vector<VkImage>            m_images;
    bool                            m_vsync;
};
} // namespace zen::val