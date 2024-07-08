#include "Graphics/Val/Swapchain.h"
#include "Graphics/Val/VulkanStrings.h"
#include "Common/Errors.h"

namespace zen::val
{
Swapchain::Swapchain(const Device& device,
                     VkSurfaceKHR surface,
                     VkExtent2D extent,
                     VkSwapchainKHR oldSwapchain,
                     uint32_t imageCount,
                     bool vsync) :
    DeviceObject(device), m_surface(surface), m_extent(extent), m_vsync(vsync)
{
    VkSurfaceCapabilitiesKHR surfaceCapabilities{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_device.GetPhysicalDeviceHandle(), m_surface,
                                              &surfaceCapabilities);

    uint32_t numPresentModes = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_device.GetPhysicalDeviceHandle(), m_surface,
                                              &numPresentModes, nullptr);
    m_presentModes.resize(numPresentModes);
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_device.GetPhysicalDeviceHandle(), m_surface,
                                              &numPresentModes, m_presentModes.data());

    LOGI("Available present modes:")
    for (auto& presentMode : m_presentModes)
    {
        LOGI("  \t{}", VkToString(presentMode));
    }

    m_surfaceFormat = ChooseSurfaceFormat();
    m_usage         = ChooseImageUsage(surfaceCapabilities.supportedUsageFlags);

    VkSwapchainCreateInfoKHR swapchainCI{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swapchainCI.surface          = m_surface;
    swapchainCI.minImageCount    = std::max(surfaceCapabilities.minImageCount,
                                            std::min(surfaceCapabilities.maxImageCount, imageCount));
    swapchainCI.imageExtent      = m_extent;
    swapchainCI.imageArrayLayers = 1;
    swapchainCI.preTransform     = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    swapchainCI.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchainCI.imageFormat      = m_surfaceFormat.format;
    swapchainCI.imageColorSpace  = m_surfaceFormat.colorSpace;
    swapchainCI.imageUsage       = m_usage;
    swapchainCI.presentMode      = ChoosePresentMode(vsync);
    swapchainCI.compositeAlpha   = ChooseCompositeAlpha(VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                                        surfaceCapabilities.supportedCompositeAlpha);
    swapchainCI.oldSwapchain     = oldSwapchain;

    CHECK_VK_ERROR_AND_THROW(
        vkCreateSwapchainKHR(m_device.GetHandle(), &swapchainCI, nullptr, &m_handle),
        "Failed to create swapchain");

    if (oldSwapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(m_device.GetHandle(), oldSwapchain, nullptr);
    }
    // get images
    uint32_t numImages = 0;
    vkGetSwapchainImagesKHR(m_device.GetHandle(), m_handle, &numImages, nullptr);
    m_images.resize(numImages);
    vkGetSwapchainImagesKHR(m_device.GetHandle(), m_handle, &numImages, m_images.data());
}

VkImageUsageFlags Swapchain::ChooseImageUsage(VkImageUsageFlags supportedUsage)
{
    static const std::vector<VkImageUsageFlagBits> defaultImageUsageFlags = {
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VkFormatProperties formatProperties;
    vkGetPhysicalDeviceFormatProperties(m_device.GetPhysicalDeviceHandle(), m_surfaceFormat.format,
                                        &formatProperties);

    VkImageUsageFlags composedUsage{0};
    std::string usageStr;

    auto valid_format_feature = [&](VkImageUsageFlagBits usageFlag) {
        if (usageFlag == VK_IMAGE_USAGE_STORAGE_BIT)
        {
            return (VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT & formatProperties.optimalTilingFeatures) !=
                0;
        }
        else
        {
            return true;
        }
    };
    for (auto usageFlag : defaultImageUsageFlags)
    {
        if ((usageFlag & supportedUsage) && valid_format_feature(usageFlag))
        {
            composedUsage |= usageFlag;
            usageStr += VkToString<VkImageUsageFlagBits>(usageFlag) + " ";
        }
    }
    LOGI("(Swapchain) Image usage flags: {}", usageStr);
    return composedUsage;
}

VkSurfaceFormatKHR Swapchain::ChooseSurfaceFormat()
{
    uint32_t numSurfaceFormats = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_device.GetPhysicalDeviceHandle(), m_surface,
                                         &numSurfaceFormats, nullptr);
    m_surfaceFormats.resize(numSurfaceFormats);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_device.GetPhysicalDeviceHandle(), m_surface,
                                         &numSurfaceFormats, m_surfaceFormats.data());
    LOGI("Available surface formats:")
    for (auto& surfaceFormat : m_surfaceFormats)
    {
        LOGI("  \t{}", VkToString(surfaceFormat));
    }
    // return first combination (best)
    return m_surfaceFormats[0];
}

VkPresentModeKHR Swapchain::ChoosePresentMode(bool vsync)
{
    if (!vsync)
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    static const std::vector<VkPresentModeKHR> presentModePriorityList = {
        VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR};
    for (const auto& presentMode : presentModePriorityList)
    {
        if (std::find(m_presentModes.begin(), m_presentModes.end(), presentMode) !=
            m_presentModes.end())
        {
            return presentMode;
        }
    }
    return VK_PRESENT_MODE_IMMEDIATE_KHR;
}

VkResult Swapchain::AcquireNextImage(uint32_t& imageIndex,
                                     VkSemaphore imageAcquiredSem,
                                     VkFence fence)
{
    return vkAcquireNextImageKHR(m_device.GetHandle(), m_handle,
                                 std::numeric_limits<uint64_t>::max(), imageAcquiredSem, fence,
                                 &imageIndex);
}

VkCompositeAlphaFlagBitsKHR Swapchain::ChooseCompositeAlpha(VkCompositeAlphaFlagBitsKHR request,
                                                            VkCompositeAlphaFlagsKHR supported)
{
    if ((request & supported) != 0)
    {
        return request;
    }

    static const std::vector<VkCompositeAlphaFlagBitsKHR> compositeAlphaFlags = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};

    for (VkCompositeAlphaFlagBitsKHR compositeAlpha : compositeAlphaFlags)
    {
        if ((compositeAlpha & supported) != 0)
        {
            LOGW("(Swapchain) Composite alpha '{}' not supported. Selecting '{}.",
                 VkToString(request), VkToString(compositeAlpha));
            return compositeAlpha;
        }
    }
    LOG_FATAL_ERROR("No compatible composite alpha found.");
    return VK_COMPOSITE_ALPHA_FLAG_BITS_MAX_ENUM_KHR;
}
} // namespace zen::val