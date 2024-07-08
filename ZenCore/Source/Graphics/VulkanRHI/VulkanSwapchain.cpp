#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanSwapchain.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"
#include "Common/Errors.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/Platform/VulkanPlatformCommon.h"

namespace zen::rhi
{
static VkSurfaceFormatKHR ChooseSurfaceFormat(VkPhysicalDevice gpu, VkSurfaceKHR surface)
{
    uint32_t numSurfaceFormats = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &numSurfaceFormats, nullptr);
    SmallVector<VkSurfaceFormatKHR> surfaceFormats;
    surfaceFormats.resize(numSurfaceFormats);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &numSurfaceFormats, surfaceFormats.data());
    // LOGI("Available surface formats:")
    // for (auto& surfaceFormat : surfaceFormats) { LOGI("  \t{}", VkToString(surfaceFormat)); }
    // return first combination (best)
    return surfaceFormats[0];
}

static VkImageUsageFlags ChooseImageUsage(VkPhysicalDevice gpu,
                                          VkImageUsageFlags supportedUsage,
                                          VkFormat surfaceFormat)
{
    static const std::vector<VkImageUsageFlagBits> defaultImageUsageFlags = {
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VkFormatProperties formatProperties;
    vkGetPhysicalDeviceFormatProperties(gpu, surfaceFormat, &formatProperties);

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
    LOGI("Swapchain Image usage flags: {}", usageStr);
    return composedUsage;
}

static VkPresentModeKHR ChoosePresentMode(VkPhysicalDevice gpu, VkSurfaceKHR surface, bool vsync)
{
    uint32_t numPresentModes = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &numPresentModes, nullptr);

    SmallVector<VkPresentModeKHR> presentModes;
    presentModes.resize(numPresentModes);
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &numPresentModes, presentModes.data());

    // LOGI("Available present modes:")
    // for (auto& presentMode : presentModes) { LOGI("  \t{}", VkToString(presentMode)); }

    if (!vsync)
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    static const std::vector<VkPresentModeKHR> presentModePriorityList = {
        VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR};
    for (const auto& presentMode : presentModePriorityList)
    {

        if (std::find(presentModes.begin(), presentModes.end(), presentMode) != presentModes.end())
        {
            return presentMode;
        }
    }
    return VK_PRESENT_MODE_IMMEDIATE_KHR;
}

static VkCompositeAlphaFlagBitsKHR ChooseCompositeAlpha(VkCompositeAlphaFlagBitsKHR request,
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

VulkanSwapchain::VulkanSwapchain(VulkanRHI* vkRHI,
                                 VulkanSurface* vulkanSurface,
                                 bool enableVSync,
                                 VkSwapchainKHR oldSwapchain)
{

    VkDevice device      = vkRHI->GetVkDevice();
    VkPhysicalDevice gpu = vkRHI->GetPhysicalDevice();
    VkSurfaceKHR surface = vulkanSurface->surface;

    VkSurfaceCapabilitiesKHR surfaceCapabilities{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &surfaceCapabilities);

    VkSurfaceFormatKHR surfaceFormat = ChooseSurfaceFormat(gpu, surface);

    m_format      = surfaceFormat.format;
    m_colorSpace  = surfaceFormat.colorSpace;
    m_presentMode = ChoosePresentMode(gpu, surface, enableVSync);

    VkImageUsageFlags imageUsage =
        ChooseImageUsage(gpu, surfaceCapabilities.supportedUsageFlags, surfaceFormat.format);

    VkSwapchainCreateInfoKHR swapchainCI{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swapchainCI.surface       = surface;
    swapchainCI.minImageCount = std::max(
        surfaceCapabilities.minImageCount,
        std::min(surfaceCapabilities.maxImageCount, (uint32_t)ZEN_RHI_SWAPCHAIN_IMAGE_COUNT));
    swapchainCI.imageExtent      = {vulkanSurface->width, vulkanSurface->height};
    swapchainCI.imageArrayLayers = 1;
    swapchainCI.preTransform     = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    swapchainCI.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchainCI.imageFormat      = m_format;
    swapchainCI.imageColorSpace  = m_colorSpace;
    swapchainCI.imageUsage       = imageUsage;
    swapchainCI.presentMode      = m_presentMode;
    swapchainCI.compositeAlpha   = ChooseCompositeAlpha(VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                                        surfaceCapabilities.supportedCompositeAlpha);
    swapchainCI.oldSwapchain     = oldSwapchain;

    VKCHECK(vkCreateSwapchainKHR(device, &swapchainCI, nullptr, &m_swaphchain));

    if (oldSwapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(device, oldSwapchain, nullptr);
    }

    LOGI("Swapchain surface format: {}", VkToString(surfaceFormat));
    LOGI("Swapchain present mode: {}", VkToString(m_presentMode));
    // get images
    uint32_t numImages = 0;
    vkGetSwapchainImagesKHR(device, m_swaphchain, &numImages, nullptr);
    m_swapchainImages.resize(numImages);
    vkGetSwapchainImagesKHR(device, m_swaphchain, &numImages, m_swapchainImages.data());
}


SwapchainHandle VulkanRHI::CreateSwapchain(SurfaceHandle surfaceHandle, bool enableVSync)
{
    VulkanSurface* vulkanSurface     = reinterpret_cast<VulkanSurface*>(surfaceHandle.value);
    VulkanSwapchain* vulkanSwapchain = new VulkanSwapchain(this, vulkanSurface, enableVSync);

    return SwapchainHandle(vulkanSwapchain);
}


Status VulkanRHI::ResizeSwapchain(SwapchainHandle swapchainHandle)
{
    return Status::eSuccess;
}

void VulkanRHI::DestroySwapchain(SwapchainHandle swapchainHandle)
{
    VulkanSwapchain* vulkanSwapchain = reinterpret_cast<VulkanSwapchain*>(swapchainHandle.value);
    if (vulkanSwapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(m_device->GetVkHandle(), vulkanSwapchain->GetVkHandle(), nullptr);
    }
    delete vulkanSwapchain;
}


} // namespace zen::rhi