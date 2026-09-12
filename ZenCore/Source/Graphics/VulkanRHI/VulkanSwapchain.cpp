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
namespace
{
void CheckWSIResult(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS)
    {
        if (result == VK_ERROR_DEVICE_LOST)
        {
            GVulkanRHI->BlockSubmissions();
        }
        LOG_ERROR_AND_THROW("{} failed: {}", operation, int32_t(result));
    }
}

template <typename T, typename Query> HeapVector<T> EnumerateWSI(Query query, const char* operation)
{
    HeapVector<T> values;
    VkResult result;
    do
    {
        uint32_t count = 0;
        CheckWSIResult(query(&count, nullptr), operation);
        if (count == 0)
        {
            LOG_ERROR_AND_THROW("{} returned no entries", operation);
        }
        values.resize(count);
        result = query(&count, values.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE)
        {
            CheckWSIResult(result, operation);
        }
        values.resize(count);
    } while (result == VK_INCOMPLETE);
    CheckWSIResult(result, operation);
    return values;
}

VkSurfaceFormatKHR ChooseSurfaceFormat(VkPhysicalDevice gpu, VkSurfaceKHR surface)
{
    const auto formats = EnumerateWSI<VkSurfaceFormatKHR>(
        [=](uint32_t* count, VkSurfaceFormatKHR* values) {
            return vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, count, values);
        },
        "vkGetPhysicalDeviceSurfaceFormatsKHR");
    // The renderer applies gamma itself, so prefer UNORM to avoid applying it twice.
    for (VkFormat preferred : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                               VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB})
    {
        for (const auto& format : formats)
        {
            if (format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
                (format.format == preferred || format.format == VK_FORMAT_UNDEFINED))
            {
                return {preferred, format.colorSpace};
            }
        }
    }
    LOG_ERROR_AND_THROW("Surface has no supported RGBA8 presentation format");
    return {};
}

VkPresentModeKHR ChoosePresentMode(VkPhysicalDevice gpu, VkSurfaceKHR surface, bool vsync)
{
    const auto modes = EnumerateWSI<VkPresentModeKHR>(
        [=](uint32_t* count, VkPresentModeKHR* values) {
            return vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, count, values);
        },
        "vkGetPhysicalDeviceSurfacePresentModesKHR");
    const VkPresentModeKHR priority[] = {vsync ? VK_PRESENT_MODE_MAILBOX_KHR :
                                                 VK_PRESENT_MODE_IMMEDIATE_KHR,
                                         VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_KHR};
    for (VkPresentModeKHR mode : priority)
    {
        if (std::find(modes.begin(), modes.end(), mode) != modes.end())
        {
            return mode;
        }
    }
    LOG_ERROR_AND_THROW("Surface has no supported presentation mode");
    return VK_PRESENT_MODE_FIFO_KHR;
}
} // namespace

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
            LOG_ERROR_AND_THROW("No compatible composite alpha found.");
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

    VkSwapchainKHR oldSwapchain = VK_NULL_HANDLE;
    if (pRecreateInfo != nullptr)
    {
        m_surface      = pRecreateInfo->surface;
        oldSwapchain   = pRecreateInfo->swapchain;
        *pRecreateInfo = {};
    }
    try
    {
        if (m_surface == VK_NULL_HANDLE)
        {
            WindowData windowData{static_cast<platform::GlfwWindowImpl*>(pWindowPtr)->GetHandle(),
                                  width, height};
            m_surface = VulkanPlatform::CreateSurface(GVulkanRHI->GetInstance(), &windowData);
        }
        if (m_surface == VK_NULL_HANDLE)
        {
            LOG_ERROR_AND_THROW("Failed to create a Vulkan presentation surface");
        }
        VkBool32 canPresent = VK_FALSE;
        CheckWSIResult(vkGetPhysicalDeviceSurfaceSupportKHR(
                           gpu, m_pDevice->GetGfxQueue()->GetFamilyIndex(), m_surface, &canPresent),
                       "vkGetPhysicalDeviceSurfaceSupportKHR");
        if (!canPresent)
        {
            LOG_ERROR_AND_THROW("The selected graphics queue cannot present to this surface");
        }
        VkSurfaceCapabilitiesKHR capabilities{};
        CheckWSIResult(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, m_surface, &capabilities),
                       "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        VkExtent2D extent = capabilities.currentExtent;
        if (extent.width == UINT32_MAX)
        {
            extent.width  = std::clamp(width, capabilities.minImageExtent.width,
                                       capabilities.maxImageExtent.width);
            extent.height = std::clamp(height, capabilities.minImageExtent.height,
                                       capabilities.maxImageExtent.height);
        }
        m_internalWidth  = extent.width;
        m_internalHeight = extent.height;
        // A minimized surface has no presentable extent. Keep its surface for restoration.
        if (extent.width == 0 || extent.height == 0)
        {
            if (oldSwapchain != VK_NULL_HANDLE)
            {
                vkDestroySwapchainKHR(device, oldSwapchain, nullptr);
            }
            return;
        }
        uint32_t imageCount =
            std::max(capabilities.minImageCount, GRHIFrameState.GetNumFramesInFlight());
        if (capabilities.maxImageCount != 0)
        {
            imageCount = std::min(imageCount, capabilities.maxImageCount);
        }
        if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        {
            LOG_ERROR_AND_THROW(
                "Surface does not support the transfer-destination presentation path");
        }
        const auto format = ChooseSurfaceFormat(gpu, m_surface);
        m_format          = format.format;
        m_colorSpace      = format.colorSpace;
        m_presentMode     = ChoosePresentMode(gpu, m_surface, enableVSync);
        VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        info.surface          = m_surface;
        info.minImageCount    = imageCount;
        info.imageExtent      = extent;
        info.imageArrayLayers = 1;
        info.preTransform =
            (capabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ?
            VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR :
            capabilities.currentTransform;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.imageFormat      = m_format;
        info.imageColorSpace  = m_colorSpace;
        info.imageUsage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        info.presentMode    = m_presentMode;
        info.compositeAlpha = ChooseCompositeAlpha(VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                                   capabilities.supportedCompositeAlpha);
        info.clipped        = VK_TRUE;
        info.oldSwapchain   = oldSwapchain;
        CheckWSIResult(vkCreateSwapchainKHR(device, &info, nullptr, &m_swaphchain),
                       "vkCreateSwapchainKHR");
        if (oldSwapchain != VK_NULL_HANDLE)
        {
            vkDestroySwapchainKHR(device, oldSwapchain, nullptr);
            oldSwapchain = VK_NULL_HANDLE;
        }
        m_swapchainImages = EnumerateWSI<VkImage>(
            [=, this](uint32_t* count, VkImage* images) {
                return vkGetSwapchainImagesKHR(device, m_swaphchain, count, images);
            },
            "vkGetSwapchainImagesKHR");
        m_numImages = static_cast<uint32_t>(m_swapchainImages.size());
        m_imageAcquiredSemaphores.resize(m_numImages);
        m_imageAcquiredSemaphoreSubmissionSerials.resize(m_numImages);
        for (uint32_t i = 0; i < m_numImages; ++i)
        {
            auto* semaphore              = m_pDevice->GetSemaphoreManager()->GetOrCreateSemaphore();
            m_imageAcquiredSemaphores[i] = semaphore;
            semaphore->SetDebugName(NameID(fmt::format("ImageAcquired-{}", i)));
        }
        LOGI("Swapchain: {} images, {}x{}, format {}, present mode {}", m_numImages, extent.width,
             extent.height, VkToString(format), VkToString(m_presentMode));
    }
    catch (...)
    {
        if (oldSwapchain != VK_NULL_HANDLE)
        {
            vkDestroySwapchainKHR(device, oldSwapchain, nullptr);
        }
        Destroy(nullptr);
        throw;
    }
}

int32_t VulkanSwapchain::AcquireNextImage(VulkanSemaphore** ppOutSemaphore)
{
    *ppOutSemaphore = nullptr;
    if (m_imageIndex >= 0)
    {
        LOG_ERROR_AND_THROW("Swapchain already has an acquired image");
    }
    if (GVulkanRHI->AreSubmissionsBlocked())
    {
        LOG_ERROR_AND_THROW("Cannot acquire while Vulkan submissions are blocked");
    }
    if (m_numImages == 0)
    {
        m_lastResult = VK_NOT_READY;
        return -1;
    }
    const int32_t nextSemaphore = (m_semaphoreIndex + 1) % static_cast<int32_t>(m_numImages);
    uint64_t& serial            = m_imageAcquiredSemaphoreSubmissionSerials[nextSemaphore];
    if (serial != 0 && !m_pDevice->GetGfxQueue()->WaitForSubmission(serial, UINT64_MAX))
    {
        GVulkanRHI->BlockSubmissions();
        LOG_ERROR_AND_THROW("Acquire semaphore submission {} did not complete", serial);
    }
    serial         = 0;
    uint32_t index = UINT32_MAX;
    // Finite timeout supports surfaces for which forward progress is not guaranteed.
    m_lastResult = vkAcquireNextImageKHR(m_pDevice->GetVkHandle(), m_swaphchain, 1000000000ull,
                                         m_imageAcquiredSemaphores[nextSemaphore]->GetVkHandle(),
                                         VK_NULL_HANDLE, &index);
    if (m_lastResult == VK_SUCCESS || m_lastResult == VK_SUBOPTIMAL_KHR)
    {
        if (index >= m_numImages)
        {
            GVulkanRHI->BlockSubmissions();
            LOG_ERROR_AND_THROW("Acquired image {} exceeds swapchain image count {}", index,
                                m_numImages);
        }
        m_acquiredSuboptimal = m_lastResult == VK_SUBOPTIMAL_KHR;
        m_semaphoreIndex     = nextSemaphore;
        m_imageIndex         = static_cast<int32_t>(index);
        *ppOutSemaphore      = m_imageAcquiredSemaphores[nextSemaphore];
        return m_imageIndex;
    }
    if (m_lastResult != VK_ERROR_OUT_OF_DATE_KHR && m_lastResult != VK_ERROR_SURFACE_LOST_KHR &&
        m_lastResult != VK_TIMEOUT && m_lastResult != VK_NOT_READY)
    {
        CheckWSIResult(m_lastResult, "vkAcquireNextImageKHR");
    }
    return -1;
}

void VulkanSwapchain::MarkAcquireSemaphoreSubmitted(uint64_t submissionSerial)
{
    if (m_imageIndex < 0 || submissionSerial == 0)
    {
        LOG_ERROR_AND_THROW("Cannot mark an acquire semaphore without an accepted submission");
    }
    m_imageAcquiredSemaphoreSubmissionSerials[m_semaphoreIndex] = submissionSerial;
}

bool VulkanSwapchain::Present(VulkanSemaphore* pRenderingCompleteSemaphore)
{
    if (m_imageIndex < 0 || GVulkanRHI->AreSubmissionsBlocked())
    {
        LOG_ERROR_AND_THROW("Cannot present without an acquired image and a usable device");
    }
    const uint32_t index = static_cast<uint32_t>(m_imageIndex);
    VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    info.swapchainCount   = 1;
    info.pSwapchains      = &m_swaphchain;
    info.pImageIndices    = &index;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    if (pRenderingCompleteSemaphore != nullptr)
    {
        semaphore               = pRenderingCompleteSemaphore->GetVkHandle();
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores    = &semaphore;
    }
    m_lastResult = vkQueuePresentKHR(m_pDevice->GetGfxQueue()->GetVkHandle(), &info);
    m_imageIndex = -1;
    if (m_lastResult == VK_SUCCESS && m_acquiredSuboptimal)
    {
        m_lastResult = VK_SUBOPTIMAL_KHR;
    }
    m_acquiredSuboptimal = false;
    if (m_lastResult != VK_SUCCESS && !NeedsRecreation())
    {
        GVulkanRHI->BlockSubmissions();
        CheckWSIResult(m_lastResult, "vkQueuePresentKHR");
    }
    return m_lastResult == VK_SUCCESS || m_lastResult == VK_SUBOPTIMAL_KHR;
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
        if (m_swaphchain != VK_NULL_HANDLE)
        {
            vkDestroySwapchainKHR(m_pDevice->GetVkHandle(), m_swaphchain, nullptr);
        }
        VulkanPlatform::DestroySurface(GVulkanRHI->GetInstance(), m_surface);
    }
    // Abandoned acquisition can leave a signaled semaphore. Keep manager ownership
    // until device teardown instead of recycling uncertain binary state.
    m_imageAcquiredSemaphores.clear();
    m_imageAcquiredSemaphoreSubmissionSerials.clear();
    m_swapchainImages.clear();
    m_numImages      = 0;
    m_imageIndex     = -1;
    m_semaphoreIndex = -1;
    m_swaphchain     = VK_NULL_HANDLE;
    m_surface = VK_NULL_HANDLE;
}
} // namespace zen
