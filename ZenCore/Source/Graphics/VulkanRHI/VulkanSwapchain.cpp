#include "Graphics/RHI/RHIFrameState.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanSwapchain.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"
#include "Utils/Errors.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanQueue.h"
#include "Graphics/VulkanRHI/VulkanSynchronization.h"
#include "Graphics/VulkanRHI/Platform/VulkanPlatformCommon.h"
#include <algorithm>

namespace zen
{
static VkSurfaceFormatKHR ChooseSurfaceFormat(VkPhysicalDevice gpu, VkSurfaceKHR surface)
{
    uint32_t numSurfaceFormats = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &numSurfaceFormats, nullptr);
    SmallVector<VkSurfaceFormatKHR> surfaceFormats;
    surfaceFormats.resize(numSurfaceFormats);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &numSurfaceFormats, surfaceFormats.data());

    // The current fullscreen shading path already applies gamma in shader, so prefer UNORM
    // presentation formats to avoid driver-dependent double-conversion when SRGB is also exposed.
    static constexpr VkSurfaceFormatKHR preferredSurfaceFormats[] = {
        {VK_FORMAT_B8G8R8A8_UNORM, VK_COLORSPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_R8G8B8A8_UNORM, VK_COLORSPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_B8G8R8A8_SRGB, VK_COLORSPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_R8G8B8A8_SRGB, VK_COLORSPACE_SRGB_NONLINEAR_KHR},
    };

    const VkSurfaceFormatKHR* pSelected = nullptr;

    for (const VkSurfaceFormatKHR& preferredSurfaceFormat : preferredSurfaceFormats)
    {
        for (const VkSurfaceFormatKHR& surfaceFormat : surfaceFormats)
        {
            if (surfaceFormat.format == preferredSurfaceFormat.format &&
                surfaceFormat.colorSpace == preferredSurfaceFormat.colorSpace)
            {
                pSelected = &surfaceFormat;
                break;
            }
        }

        if (pSelected != nullptr)
        {
            break;
        }
    }

    if (pSelected == nullptr)
    {
        for (const VkSurfaceFormatKHR& surfaceFormat : surfaceFormats)
        {
            if (surfaceFormat.colorSpace == VK_COLORSPACE_SRGB_NONLINEAR_KHR)
            {
                pSelected = &surfaceFormat;
                break;
            }
        }
    }

    return pSelected != nullptr ? *pSelected : surfaceFormats[0];
}

static VkImageUsageFlags ChooseImageUsage(VkPhysicalDevice gpu,
                                          VkImageUsageFlags supportedUsage,
                                          VkFormat surfaceFormat)
{
    static constexpr VkImageUsageFlagBits defaultImageUsageFlags[] = {
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VkFormatProperties formatProperties;
    vkGetPhysicalDeviceFormatProperties(gpu, surfaceFormat, &formatProperties);

    VkImageUsageFlags composedUsage{0};
    std::string usageStr;

    for (VkImageUsageFlagBits usageFlag : defaultImageUsageFlags)
    {
        if ((usageFlag & supportedUsage) &&
            (usageFlag != VK_IMAGE_USAGE_STORAGE_BIT ||
             (VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT & formatProperties.optimalTilingFeatures) != 0))
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

    VkPresentModeKHR selected = VK_PRESENT_MODE_IMMEDIATE_KHR;

    if (vsync)
    {
        static constexpr VkPresentModeKHR presentModePriorityList[] = {
            VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR};

        for (const VkPresentModeKHR& presentMode : presentModePriorityList)
        {
            if (std::find(presentModes.begin(), presentModes.end(), presentMode) !=
                presentModes.end())
            {
                selected = presentMode;
                break;
            }
        }
    }

    return selected;
}

static VkCompositeAlphaFlagBitsKHR ChooseCompositeAlpha(VkCompositeAlphaFlagBitsKHR request,
                                                        VkCompositeAlphaFlagsKHR supported)
{
    VkCompositeAlphaFlagBitsKHR selected = request;

    if ((request & supported) == 0)
    {
        selected = VK_COMPOSITE_ALPHA_FLAG_BITS_MAX_ENUM_KHR;

        static constexpr VkCompositeAlphaFlagBitsKHR compositeAlphaFlags[] = {
            VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};

        for (VkCompositeAlphaFlagBitsKHR compositeAlpha : compositeAlphaFlags)
        {
            if ((compositeAlpha & supported) != 0)
            {
                LOGW("(Swapchain) Composite alpha '{}' not supported. Selecting '{}.",
                     VkToString(request), VkToString(compositeAlpha));
                selected = compositeAlpha;
                break;
            }
        }

        if (selected == VK_COMPOSITE_ALPHA_FLAG_BITS_MAX_ENUM_KHR)
        {
            LOG_FATAL_ERROR("No compatible composite alpha found.");
        }
    }

    return selected;
}

VulkanSwapchain::VulkanSwapchain(void* pWindowPtr,
                                 uint32_t width,
                                 uint32_t height,
                                 bool enableVSync,
                                 VulkanSwapchainRecreateInfo* pRecreateInfo) :
    m_pDevice(GVulkanRHI->GetDevice())
{
    VkDevice device      = m_pDevice->GetVkHandle();
    VkPhysicalDevice gpu = GVulkanRHI->GetPhysicalDevice();

    if (pRecreateInfo != nullptr)
    {
        m_surface              = pRecreateInfo->surface;
        pRecreateInfo->surface = VK_NULL_HANDLE;
    }
    else
    {
        WindowData windowData{static_cast<platform::GlfwWindowImpl*>(pWindowPtr)->GetHandle(),
                              width, height};
        m_surface = VulkanPlatform::CreateSurface(GVulkanRHI->GetInstance(), &windowData);
    }

    VkSurfaceCapabilitiesKHR surfaceCapabilities{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, m_surface, &surfaceCapabilities);
    uint32_t swapchainWidth  = std::clamp(width, surfaceCapabilities.minImageExtent.width,
                                          surfaceCapabilities.maxImageExtent.width);
    uint32_t swapchainHeight = std::clamp(height, surfaceCapabilities.minImageExtent.height,
                                          surfaceCapabilities.maxImageExtent.height);

    const uint32_t requiredImageCount = GRHIFrameState.GetNumFramesInFlight();
    uint32_t minImageCount =
        std::max(surfaceCapabilities.minImageCount,
                 std::min(surfaceCapabilities.maxImageCount, requiredImageCount));

    VkSurfaceFormatKHR surfaceFormat = ChooseSurfaceFormat(gpu, m_surface);

    m_format      = surfaceFormat.format;
    m_colorSpace  = surfaceFormat.colorSpace;
    m_presentMode = ChoosePresentMode(gpu, m_surface, enableVSync);

    VkImageUsageFlags imageUsage =
        ChooseImageUsage(gpu, surfaceCapabilities.supportedUsageFlags, m_format);

    VkSwapchainCreateInfoKHR swapchainCI{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swapchainCI.surface          = m_surface;
    swapchainCI.minImageCount    = minImageCount;
    swapchainCI.imageExtent      = {swapchainWidth, swapchainHeight};
    swapchainCI.imageArrayLayers = 1;
    swapchainCI.preTransform     = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    swapchainCI.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchainCI.imageFormat      = m_format;
    swapchainCI.imageColorSpace  = m_colorSpace;
    swapchainCI.imageUsage       = imageUsage;
    swapchainCI.presentMode      = m_presentMode;
    swapchainCI.compositeAlpha = ChooseCompositeAlpha(VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                                      surfaceCapabilities.supportedCompositeAlpha);
    swapchainCI.oldSwapchain   = pRecreateInfo == nullptr ? nullptr : pRecreateInfo->swapchain;

    VKCHECK(vkCreateSwapchainKHR(device, &swapchainCI, nullptr, &m_swaphchain));

    if (pRecreateInfo != nullptr)
    {
        vkDestroySwapchainKHR(device, pRecreateInfo->swapchain, nullptr);
        pRecreateInfo->swapchain = VK_NULL_HANDLE;
    }

    LOGI("Swapchain surface format: {}", VkToString(surfaceFormat));
    LOGI("Swapchain present mode: {}", VkToString(m_presentMode));
    // get images
    uint32_t availableImagesCount = 0;
    vkGetSwapchainImagesKHR(device, m_swaphchain, &availableImagesCount, nullptr);

    m_numImages = std::min(availableImagesCount, ZEN_MAX_NUM_SWAPCHAIN_IMAGES);
    // m_swapchainImages.resize(numImages);
    vkGetSwapchainImagesKHR(device, m_swaphchain, &m_numImages, m_swapchainImages);

    // create semaphores
    // m_imageAcquiredSemaphores.reserve(numImages);
    for (uint32_t i = 0; i < m_numImages; i++)
    {
        VulkanSemaphore* pSemaphore = m_pDevice->GetSemaphoreManager()->GetOrCreateSemaphore();
        const NameID debugName(fmt::format("ImageAcquired-{}", i));
        pSemaphore->SetDebugName(debugName);
        m_pImageAcquiredSemaphores[i] = pSemaphore;
    }

    m_internalWidth  = std::min(width, swapchainCI.imageExtent.width);
    m_internalHeight = std::min(height, swapchainCI.imageExtent.height);
}

int32_t VulkanSwapchain::AcquireNextImage(VulkanSemaphore** ppOutSemaphore)
{
    VERIFY_EXPR(m_imageIndex == -1);
    const int32_t prevSemaphoreIndex = m_semaphoreIndex;
    m_semaphoreIndex                 = (m_semaphoreIndex + 1) % static_cast<int32_t>(m_numImages);
    const uint64_t pendingSubmissionSerial =
        m_imageAcquiredSemaphoreSubmissionSerials[m_semaphoreIndex];
    bool ready = true;

    if (pendingSubmissionSerial != 0)
    {
        // Binary acquire semaphores cannot be reused while their queue wait is still pending.
        ready = m_pDevice->GetGfxQueue()->WaitForSubmission(pendingSubmissionSerial, UINT64_MAX);

        if (ready)
        {
            m_imageAcquiredSemaphoreSubmissionSerials[m_semaphoreIndex] = 0;
        }
        else
        {
            LOGE("Vulkan swapchain: acquire semaphore submission {} did not complete",
                 pendingSubmissionSerial);
        }
    }

    int32_t acquiredImage = -1;

    if (ready)
    {
        uint32_t imageIndex = 0;
        VkResult result;

        do
        {
            result =
                vkAcquireNextImageKHR(m_pDevice->GetVkHandle(), m_swaphchain, UINT64_MAX,
                                      m_pImageAcquiredSemaphores[m_semaphoreIndex]->GetVkHandle(),
                                      VK_NULL_HANDLE, &imageIndex);
        } while (imageIndex >= m_numImages &&
                 (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR));

        if (result != VK_ERROR_OUT_OF_DATE_KHR && result != VK_ERROR_SURFACE_LOST_KHR)
        {
            *ppOutSemaphore = m_pImageAcquiredSemaphores[m_semaphoreIndex];
            m_imageIndex    = static_cast<int32_t>(imageIndex);
            acquiredImage   = m_imageIndex;
        }
    }

    if (acquiredImage < 0)
    {
        m_semaphoreIndex = prevSemaphoreIndex;
    }

    return acquiredImage;
}

void VulkanSwapchain::MarkAcquireSemaphoreSubmitted(uint64_t submissionSerial)
{
    VERIFY_EXPR(m_semaphoreIndex >= 0 && m_semaphoreIndex < static_cast<int32_t>(m_numImages));
    m_imageAcquiredSemaphoreSubmissionSerials[m_semaphoreIndex] = submissionSerial;
}

bool VulkanSwapchain::Present(VulkanSemaphore* pRenderingCompleteSemaphore)
{
    bool returnValue{};

    VkPresentInfoKHR presentInfo;
    InitVkStruct(presentInfo, VK_STRUCTURE_TYPE_PRESENT_INFO_KHR);
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains    = &m_swaphchain;
    presentInfo.pImageIndices  = reinterpret_cast<uint32_t*>(&m_imageIndex);
    VkSemaphore semaphore{VK_NULL_HANDLE};

    if (pRenderingCompleteSemaphore != nullptr)
    {
        presentInfo.waitSemaphoreCount = 1;
        semaphore                      = pRenderingCompleteSemaphore->GetVkHandle();
        presentInfo.pWaitSemaphores    = &semaphore;
    }

    VkResult result = vkQueuePresentKHR(m_pDevice->GetGfxQueue()->GetVkHandle(), &presentInfo);
    m_imageIndex    = -1;

    if (!(result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR))
    {
        returnValue = true;
    }

    return returnValue;
}

void VulkanSwapchain::Destroy(VulkanSwapchainRecreateInfo* pRecreateInfo)
{
    if (pRecreateInfo != nullptr)
    {
        pRecreateInfo->swapchain = m_swaphchain;
        pRecreateInfo->surface   = m_surface;
    }
    else
    {
        vkDestroySwapchainKHR(m_pDevice->GetVkHandle(), m_swaphchain, nullptr);
    }

    m_swaphchain = VK_NULL_HANDLE;

    for (uint32_t i = 0; i < m_numImages; i++)
    {
        // m_imageAcquiredSemaphores[i]->Release();
        m_pDevice->GetSemaphoreManager()->ReleaseSemaphore(m_pImageAcquiredSemaphores[i]);
        m_pImageAcquiredSemaphores[i]                = nullptr;
        m_imageAcquiredSemaphoreSubmissionSerials[i] = 0;
    }

    // m_imageAcquiredSemaphores.clear();
    if (pRecreateInfo == nullptr)
    {
        VulkanPlatform::DestroySurface(GVulkanRHI->GetInstance(), m_surface);
    }

    m_surface = VK_NULL_HANDLE;
}
} // namespace zen
