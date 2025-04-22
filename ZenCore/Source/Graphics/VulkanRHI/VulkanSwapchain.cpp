#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanSwapchain.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"
#include "Common/Errors.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanQueue.h"
#include "Graphics/VulkanRHI/VulkanSynchronization.h"
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

VulkanSwapchain::VulkanSwapchain(VulkanRHI* RHI,
                                 void* windowPtr,
                                 uint32_t width,
                                 uint32_t height,
                                 bool enableVSync,
                                 VulkanSwapchainRecreateInfo* recreateInfo) :
    m_RHI(RHI), m_device(RHI->GetDevice())
{

    VkDevice device      = m_RHI->GetDevice()->GetVkHandle();
    VkPhysicalDevice gpu = m_RHI->GetPhysicalDevice();
    if (recreateInfo != nullptr)
    {
        m_surface             = recreateInfo->surface;
        recreateInfo->surface = VK_NULL_HANDLE;
    }
    else
    {
        WindowData windowData{static_cast<platform::GlfwWindowImpl*>(windowPtr)->GetHandle(), width,
                              height};
        m_surface = VulkanPlatform::CreateSurface(m_RHI->GetInstance(), &windowData);
    }

    VkSurfaceCapabilitiesKHR surfaceCapabilities{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, m_surface, &surfaceCapabilities);
    uint32_t swapchainWidth  = surfaceCapabilities.currentExtent.width == 0xFFFFFFFF ?
         width :
         surfaceCapabilities.currentExtent.width;
    uint32_t swapchainHeight = surfaceCapabilities.currentExtent.height == 0xFFFFFFFF ?
        height :
        surfaceCapabilities.currentExtent.height;

    VkSurfaceFormatKHR surfaceFormat = ChooseSurfaceFormat(gpu, m_surface);

    m_format      = surfaceFormat.format;
    m_colorSpace  = surfaceFormat.colorSpace;
    m_presentMode = ChoosePresentMode(gpu, m_surface, enableVSync);

    VkImageUsageFlags imageUsage =
        ChooseImageUsage(gpu, surfaceCapabilities.supportedUsageFlags, m_format);

    VkSwapchainCreateInfoKHR swapchainCI{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swapchainCI.surface       = m_surface;
    swapchainCI.minImageCount = std::max(
        surfaceCapabilities.minImageCount,
        std::min(surfaceCapabilities.maxImageCount, (uint32_t)ZEN_RHI_SWAPCHAIN_IMAGE_COUNT));
    swapchainCI.imageExtent      = {width, height};
    swapchainCI.imageArrayLayers = 1;
    swapchainCI.preTransform     = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    swapchainCI.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchainCI.imageFormat      = m_format;
    swapchainCI.imageColorSpace  = m_colorSpace;
    swapchainCI.imageUsage       = imageUsage;
    swapchainCI.presentMode      = m_presentMode;
    swapchainCI.compositeAlpha   = ChooseCompositeAlpha(VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                                        surfaceCapabilities.supportedCompositeAlpha);
    swapchainCI.oldSwapchain     = recreateInfo == nullptr ? nullptr : recreateInfo->swapchain;

    VKCHECK(vkCreateSwapchainKHR(device, &swapchainCI, nullptr, &m_swaphchain));

    if (recreateInfo != nullptr)
    {
        vkDestroySwapchainKHR(device, recreateInfo->swapchain, nullptr);
        recreateInfo->swapchain = VK_NULL_HANDLE;
    }

    LOGI("Swapchain surface format: {}", VkToString(surfaceFormat));
    LOGI("Swapchain present mode: {}", VkToString(m_presentMode));
    // get images
    uint32_t numImages = 0;
    vkGetSwapchainImagesKHR(device, m_swaphchain, &numImages, nullptr);
    m_swapchainImages.resize(numImages);
    vkGetSwapchainImagesKHR(device, m_swaphchain, &numImages, m_swapchainImages.data());
    // create semaphores
    m_imageAcquiredSemphores.reserve(numImages);
    for (uint32_t i = 0; i < numImages; i++)
    {
        auto* semaphore = m_device->GetSemaphoreManager()->GetOrCreateSemaphore();
        // auto* semaphore = new VulkanSemaphore(m_device);
        const std::string debugName = "ImageAcquired-" + std::to_string(i);
        semaphore->SetDebugName(debugName.c_str());
        m_imageAcquiredSemphores.push_back(semaphore);
    }

    m_internalWidth  = std::min(width, swapchainCI.imageExtent.width);
    m_internalHeight = std::min(height, swapchainCI.imageExtent.height);
}

int32_t VulkanSwapchain::AcquireNextImage(VulkanSemaphore** outSemaphore)
{
    VERIFY_EXPR(m_imageIndex == -1);
    const int32_t prevSemaphoreIndex = m_semaphoreIndex;
    m_semaphoreIndex =
        (m_semaphoreIndex + 1) % static_cast<int32_t>(m_imageAcquiredSemphores.size());
    uint32_t imageIndex = 0;
    VkResult result;
    {
        result = vkAcquireNextImageKHR(m_device->GetVkHandle(), m_swaphchain, UINT64_MAX,
                                       m_imageAcquiredSemphores[m_semaphoreIndex]->GetVkHandle(),
                                       VK_NULL_HANDLE, &imageIndex);
        const uint32_t maxImageIndex = m_imageAcquiredSemphores.size() - 1;
        while (imageIndex > maxImageIndex && (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR))
        {
            result =
                vkAcquireNextImageKHR(m_device->GetVkHandle(), m_swaphchain, UINT64_MAX,
                                      m_imageAcquiredSemphores[m_semaphoreIndex]->GetVkHandle(),
                                      VK_NULL_HANDLE, &imageIndex);
        }
    }
    if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        m_semaphoreIndex = prevSemaphoreIndex;
        return -1;
    }
    if (result == VK_ERROR_SURFACE_LOST_KHR)
    {
        m_semaphoreIndex = prevSemaphoreIndex;
        return -1;
    }
    *outSemaphore = m_imageAcquiredSemphores[m_semaphoreIndex];
    m_imageIndex  = static_cast<int32_t>(imageIndex);
    return m_imageIndex;
}

bool VulkanSwapchain::Present(VulkanSemaphore* renderingCompleteSemaphore)
{
    VkPresentInfoKHR presentInfo;
    InitVkStruct(presentInfo, VK_STRUCTURE_TYPE_PRESENT_INFO_KHR);
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains    = &m_swaphchain;
    presentInfo.pImageIndices  = reinterpret_cast<uint32_t*>(&m_imageIndex);
    VkSemaphore semaphore{VK_NULL_HANDLE};
    if (renderingCompleteSemaphore != nullptr)
    {
        presentInfo.waitSemaphoreCount = 1;
        semaphore                      = renderingCompleteSemaphore->GetVkHandle();
        presentInfo.pWaitSemaphores    = &semaphore;
    }
    VkResult result = vkQueuePresentKHR(m_device->GetGfxQueue()->GetVkHandle(), &presentInfo);
    m_imageIndex    = -1;
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
    {
        return false;
    }
    return true;
}

void VulkanSwapchain::Destroy(VulkanSwapchainRecreateInfo* recreateInfo)
{
    if (recreateInfo != nullptr)
    {
        recreateInfo->swapchain = m_swaphchain;
        recreateInfo->surface   = m_surface;
    }
    else
    {
        vkDestroySwapchainKHR(m_device->GetVkHandle(), m_swaphchain, nullptr);
    }
    m_swaphchain = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < m_imageAcquiredSemphores.size(); i++)
    {
        // m_imageAcquiredSemphores[i]->Release();
        m_device->GetSemaphoreManager()->ReleaseSemaphore(m_imageAcquiredSemphores[i]);
    }
    m_imageAcquiredSemphores.clear();
    if (recreateInfo == nullptr)
    {
        VulkanPlatform::DestroySurface(m_RHI->GetInstance(), m_surface);
    }
    m_surface = VK_NULL_HANDLE;
}
} // namespace zen::rhi