#include "Graphics/Val/Device.h"
#include "Graphics/Val/Swapchain.h"
#include "Graphics/Val/VulkanStrings.h"
#include "Common/Errors.h"

namespace zen::val
{
Swapchain::Swapchain(Device& device, VkSurfaceKHR surface, VkExtent2D extent, VkSwapchainKHR oldSwapchain, uint32_t imageCount, bool vsync) :
    m_device(device), m_surface(surface), m_extent(extent), m_vsync(vsync)
{
    const std::vector<VkSurfaceFormatKHR> surfaceFormatPriorityList = {{VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
                                                                       {VK_FORMAT_B8G8R8A8_UNORM, VK_COLORSPACE_SRGB_NONLINEAR_KHR}};
    VkSurfaceCapabilitiesKHR              surfaceCapabilities{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_device.GetPhysicalDeviceHandle(), m_surface, &surfaceCapabilities);

    uint32_t numSurfaceFormats = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_device.GetPhysicalDeviceHandle(), m_surface, &numSurfaceFormats, nullptr);
    m_surfaceFormats.resize(numSurfaceFormats);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_device.GetPhysicalDeviceHandle(), m_surface, &numSurfaceFormats, m_surfaceFormats.data());

    uint32_t numPresentModes = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_device.GetPhysicalDeviceHandle(), m_surface, &numPresentModes, nullptr);
    m_presentModes.resize(numPresentModes);
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_device.GetPhysicalDeviceHandle(), m_surface, &numPresentModes, m_presentModes.data());

    LOGI("Available surface formats:")
    for (auto& surfaceFormat : m_surfaceFormats)
    {
        LOGI("  \t{}", VkToString(surfaceFormat));
    }
    LOGI("Available present modes:")
    for (auto& presentMode : m_presentModes)
    {
        LOGI("  \t{}", VkToString(presentMode));
    }

    VkSwapchainCreateInfoKHR swapchainCI{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swapchainCI.surface          = m_surface;
    swapchainCI.minImageCount    = std::clamp(imageCount, surfaceCapabilities.minImageCount, surfaceCapabilities.maxImageCount);
    swapchainCI.imageExtent      = m_extent;
    swapchainCI.imageArrayLayers = 1;
    swapchainCI.preTransform     = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    swapchainCI.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchainCI.imageFormat      = m_surfaceFormats[0].format;
    swapchainCI.imageColorSpace  = m_surfaceFormats[0].colorSpace;
    swapchainCI.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swapchainCI.presentMode      = ChoosePresentMode(vsync);
    swapchainCI.compositeAlpha   = ChooseCompositeAlpha(VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, surfaceCapabilities.supportedCompositeAlpha);
    swapchainCI.oldSwapchain     = oldSwapchain;

    CHECK_VK_ERROR_AND_THROW(vkCreateSwapchainKHR(m_device.GetHandle(), &swapchainCI, nullptr, &m_handle), "Failed to create swapchain");

    // get images
    uint32_t numImages = 0;
    vkGetSwapchainImagesKHR(m_device.GetHandle(), m_handle, &numImages, nullptr);
    m_images.resize(numImages);
    vkGetSwapchainImagesKHR(m_device.GetHandle(), m_handle, &numImages, m_images.data());
}

VkPresentModeKHR Swapchain::ChoosePresentMode(bool vsync)
{
    if (!vsync)
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    static const std::vector<VkPresentModeKHR> presentModePriorityList = {VK_PRESENT_MODE_MAILBOX_KHR,
                                                                          VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR};
    for (const auto& presentMode : presentModePriorityList)
    {
        if (std::find(m_presentModes.begin(), m_presentModes.end(), presentMode) != m_presentModes.end())
        {
            return presentMode;
        }
    }
    return VK_PRESENT_MODE_IMMEDIATE_KHR;
}

Swapchain::~Swapchain()
{
    if (m_handle != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(m_device.GetHandle(), m_handle, nullptr);
    }
    if (m_surface != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(m_device.GetInstanceHandle(), m_surface, nullptr);
    }
}

VkCompositeAlphaFlagBitsKHR Swapchain::ChooseCompositeAlpha(VkCompositeAlphaFlagBitsKHR request, VkCompositeAlphaFlagsKHR supported)
{
    if ((request & supported) != 0)
    {
        return request;
    }

    static const std::vector<VkCompositeAlphaFlagBitsKHR> compositeAlphaFlags = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};

    for (VkCompositeAlphaFlagBitsKHR compositeAlpha : compositeAlphaFlags)
    {
        if ((compositeAlpha & supported) != 0)
        {
            LOGW("(Swapchain) Composite alpha '{}' not supported. Selecting '{}.", VkToString(request), VkToString(compositeAlpha));
            return compositeAlpha;
        }
    }
    LOG_FATAL_ERROR("No compatible composite alpha found.");
    return VK_COMPOSITE_ALPHA_FLAG_BITS_MAX_ENUM_KHR;
}
} // namespace zen::val