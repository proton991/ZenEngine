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
    const HeapVector<VkSurfaceFormatKHR> formats = EnumerateWSI<VkSurfaceFormatKHR>(
        [=](uint32_t* count, VkSurfaceFormatKHR* values) {
            return vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, count, values);
        },
        "vkGetPhysicalDeviceSurfaceFormatsKHR");
    VkSurfaceFormatKHR selected{};
    bool found = false;
    // The renderer applies gamma itself, so prefer UNORM to avoid applying it twice.
    for (VkFormat preferred : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                               VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB})
    {
        for (const VkSurfaceFormatKHR& format : formats)
        {
            if (format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
                (format.format == preferred || format.format == VK_FORMAT_UNDEFINED))
            {
                selected = {preferred, format.colorSpace};
                found    = true;
                break;
            }
        }
        if (found)
        {
            break;
        }
    }
    if (!found)
    {
        LOG_ERROR_AND_THROW("Surface has no supported RGBA8 presentation format");
    }
    return selected;
}

VkPresentModeKHR ChoosePresentMode(VkPhysicalDevice gpu, VkSurfaceKHR surface, bool vsync)
{
    const HeapVector<VkPresentModeKHR> modes = EnumerateWSI<VkPresentModeKHR>(
        [=](uint32_t* count, VkPresentModeKHR* values) {
            return vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, count, values);
        },
        "vkGetPhysicalDeviceSurfacePresentModesKHR");
    const VkPresentModeKHR priority[] = {vsync ? VK_PRESENT_MODE_MAILBOX_KHR :
                                                 VK_PRESENT_MODE_IMMEDIATE_KHR,
                                         VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_KHR};
    VkPresentModeKHR selected         = VK_PRESENT_MODE_FIFO_KHR;
    bool found                        = false;
    for (VkPresentModeKHR mode : priority)
    {
        if (std::find(modes.begin(), modes.end(), mode) != modes.end())
        {
            selected = mode;
            found    = true;
            break;
        }
    }
    if (!found)
    {
        LOG_ERROR_AND_THROW("Surface has no supported presentation mode");
    }
    return selected;
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

void VulkanSwapchain::DestroyOldSwapchain(VkSwapchainKHR& oldSwapchain)
{
    if (oldSwapchain != VK_NULL_HANDLE)
    {
        const bool retained = std::any_of(m_retiredSwapchains.begin(), m_retiredSwapchains.end(),
                                          [oldSwapchain](const VulkanRetiredSwapchain& retired) {
                                              return retired.swapchain == oldSwapchain;
                                          });
        if (!retained)
        {
            vkDestroySwapchainKHR(m_pDevice->GetVkHandle(), oldSwapchain, nullptr);
        }
        oldSwapchain = VK_NULL_HANDLE;
    }
}

VulkanSwapchain::VulkanSwapchain(uint32_t width,
                                 uint32_t height,
                                 bool enableVSync,
                                 VulkanSwapchainRecreateInfo* pRecreateInfo) :
    m_pDevice(GVulkanRHI->GetDevice())
{
    VkDevice device      = m_pDevice->GetVkHandle();
    VkPhysicalDevice gpu = GVulkanRHI->GetPhysicalDevice();
    m_hasPresentFences   = m_pDevice->GetExtensionFlags().hasSwapchainMaintenance1;

    VkSwapchainKHR oldSwapchain = VK_NULL_HANDLE;
    if (pRecreateInfo != nullptr)
    {
        m_surface           = pRecreateInfo->surface;
        oldSwapchain        = pRecreateInfo->swapchain;
        m_retiredSwapchains = std::move(pRecreateInfo->retiredSwapchains);
        *pRecreateInfo      = {};
    }
    try
    {
        ASSERT(m_surface != VK_NULL_HANDLE);
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
            // Preserve the native oldSwapchain link while minimized: a fallback
            // predecessor may still be the surface's non-retired swapchain.
            m_swapchain  = oldSwapchain;
            oldSwapchain = VK_NULL_HANDLE;
        }
        else
        {
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
            const VkSurfaceFormatKHR format = ChooseSurfaceFormat(gpu, m_surface);
            m_format                        = format.format;
            m_colorSpace                    = format.colorSpace;
            m_presentMode                   = ChoosePresentMode(gpu, m_surface, enableVSync);
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
            CheckWSIResult(vkCreateSwapchainKHR(device, &info, nullptr, &m_swapchain),
                           "vkCreateSwapchainKHR");
            DestroyOldSwapchain(oldSwapchain);
            m_swapchainImages = EnumerateWSI<VkImage>(
                [=, this](uint32_t* count, VkImage* images) {
                    return vkGetSwapchainImagesKHR(device, m_swapchain, count, images);
                },
                "vkGetSwapchainImagesKHR");
            m_numImages = static_cast<uint32_t>(m_swapchainImages.size());
            m_acquireSync.resize(m_numImages);
            m_presentSync.resize(m_numImages);
            VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            for (uint32_t i = 0; i < m_numImages; ++i)
            {
                AcquireSync& acquire = m_acquireSync[i];
                acquire.semaphore    = m_pDevice->GetSemaphoreManager()->GetOrCreateSemaphore();
                acquire.semaphore->SetDebugName(NameID(fmt::format("ImageAcquired-{}", i)));
                CheckWSIResult(vkCreateFence(device, &fenceInfo, nullptr, &acquire.fence),
                               "vkCreateFence(acquire)");
                PresentSync& present = m_presentSync[i];
                present.semaphore    = m_pDevice->GetSemaphoreManager()->GetOrCreateSemaphore();
                present.semaphore->SetDebugName(NameID(fmt::format("RenderComplete-{}", i)));
                if (m_hasPresentFences)
                {
                    CheckWSIResult(vkCreateFence(device, &fenceInfo, nullptr, &present.fence),
                                   "vkCreateFence(present)");
                }
            }
            LOGI("Swapchain: {} images, {}x{}, format {}, present mode {}", m_numImages,
                 extent.width, extent.height, VkToString(format), VkToString(m_presentMode));
        }
    }
    catch (...)
    {
        DestroyOldSwapchain(oldSwapchain);
        Destroy(nullptr);
        throw;
    }
}

int32_t VulkanSwapchain::AcquireNextImage(VulkanSemaphore** ppOutSemaphore)
{
    int32_t acquiredIndex = -1;
    *ppOutSemaphore       = nullptr;
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
    }
    else
    {
        const int32_t nextSemaphore = (m_semaphoreIndex + 1) % static_cast<int32_t>(m_numImages);
        for (AcquireSync& sync : m_acquireSync)
        {
            CompleteAcquire(sync, false);
        }
        AcquireSync& acquire = m_acquireSync[nextSemaphore];
        CompleteAcquire(acquire, true);
        if (GVulkanRHI->AreSubmissionsBlocked())
        {
            LOG_ERROR_AND_THROW("Acquire fence completion reported device loss");
        }
        uint64_t& serial = acquire.submissionSerial;
        if (serial != 0 && !m_pDevice->GetGfxQueue()->WaitForCompletion(serial, UINT64_MAX))
        {
            GVulkanRHI->BlockSubmissions();
            LOG_ERROR_AND_THROW("Acquire semaphore submission {} did not complete", serial);
        }
        serial         = 0;
        uint32_t index = UINT32_MAX;
        // Finite timeout supports surfaces for which forward progress is not guaranteed.
        m_lastResult =
            vkAcquireNextImageKHR(m_pDevice->GetVkHandle(), m_swapchain, 1000000000ull,
                                  acquire.semaphore->GetVkHandle(), acquire.fence, &index);
        if (m_lastResult == VK_SUCCESS || m_lastResult == VK_SUBOPTIMAL_KHR)
        {
            acquire.pending = true;
            if (index >= m_numImages)
            {
                GVulkanRHI->BlockSubmissions();
                LOG_ERROR_AND_THROW("Acquired image {} exceeds swapchain image count {}", index,
                                    m_numImages);
            }
            m_acquiredSuboptimal = m_lastResult == VK_SUBOPTIMAL_KHR;
            m_semaphoreIndex     = nextSemaphore;
            m_imageIndex         = static_cast<int32_t>(index);
            acquire.imageIndex   = index;
            acquire.previousPresentSerial =
                m_presentSync[index].pending ? m_presentSync[index].serial : 0;
            // A presentation fence must finish before it is reused, independently of
            // graphics submission completion or acquisition of a different image.
            if (m_hasPresentFences)
            {
                WaitForPresent(m_presentSync[index]);
                if (GVulkanRHI->AreSubmissionsBlocked())
                {
                    LOG_ERROR_AND_THROW("Presentation fence completion reported device loss");
                }
            }
            *ppOutSemaphore = acquire.semaphore;
            acquiredIndex   = m_imageIndex;
        }
        else if (m_lastResult != VK_ERROR_OUT_OF_DATE_KHR &&
                 m_lastResult != VK_ERROR_SURFACE_LOST_KHR && m_lastResult != VK_TIMEOUT &&
                 m_lastResult != VK_NOT_READY)
        {
            CheckWSIResult(m_lastResult, "vkAcquireNextImageKHR");
        }
    }
    return acquiredIndex;
}

void VulkanSwapchain::MarkAcquireSemaphoreSubmitted(uint64_t submissionSerial)
{
    if (m_imageIndex < 0 || submissionSerial == 0)
    {
        LOG_ERROR_AND_THROW("Cannot mark an acquire semaphore without an accepted submission");
    }
    m_acquireSync[m_semaphoreIndex].submissionSerial = submissionSerial;
}

bool VulkanSwapchain::Present(VulkanSemaphore* pRenderingCompleteSemaphore)
{
    if (m_imageIndex < 0 || GVulkanRHI->AreSubmissionsBlocked())
    {
        LOG_ERROR_AND_THROW("Cannot present without an acquired image and a usable device");
    }
    const uint32_t index = static_cast<uint32_t>(m_imageIndex);
    VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    info.swapchainCount = 1;
    info.pSwapchains    = &m_swapchain;
    info.pImageIndices  = &index;
    PresentSync& sync   = m_presentSync[index];
    VkSwapchainPresentFenceInfoEXT fenceInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT};
    if (m_hasPresentFences)
    {
        fenceInfo.swapchainCount = 1;
        fenceInfo.pFences        = &sync.fence;
        info.pNext               = &fenceInfo;
    }
    VkSemaphore semaphore = VK_NULL_HANDLE;
    if (pRenderingCompleteSemaphore != nullptr)
    {
        semaphore               = pRenderingCompleteSemaphore->GetVkHandle();
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores    = &semaphore;
    }
    m_lastResult = vkQueuePresentKHR(m_pDevice->GetGfxQueue()->GetVkHandle(), &info);
    // These WSI errors still enqueue the semaphore wait and presentation fence.
    // Allocation failures reject the operation, leaving the fence unsignaled.
    sync.pending = m_lastResult == VK_SUCCESS || m_lastResult == VK_SUBOPTIMAL_KHR ||
        m_lastResult == VK_ERROR_OUT_OF_DATE_KHR || m_lastResult == VK_ERROR_SURFACE_LOST_KHR;
    if (sync.pending)
    {
        ++sync.serial;
    }
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

void VulkanSwapchain::ReleaseRetiredSwapchains()
{
    for (VulkanRetiredSwapchain& retired : m_retiredSwapchains)
    {
        for (VulkanSemaphore*& semaphore : retired.presentationSemaphores)
        {
            m_pDevice->GetSemaphoreManager()->DestroySemaphore(semaphore);
        }
        vkDestroySwapchainKHR(m_pDevice->GetVkHandle(), retired.swapchain, nullptr);
    }
    m_retiredSwapchains.clear();
}

void VulkanSwapchain::CompleteAcquire(AcquireSync& sync, bool wait)
{
    if (sync.pending)
    {
        const VkDevice device = m_pDevice->GetVkHandle();
        const VkResult result = wait ?
            vkWaitForFences(device, 1, &sync.fence, VK_TRUE, UINT64_MAX) :
            vkGetFenceStatus(device, sync.fence);
        if (result == VK_ERROR_DEVICE_LOST)
        {
            GVulkanRHI->BlockSubmissions();
            sync.pending = false;
        }
        else if (result != VK_NOT_READY)
        {
            CheckWSIResult(result, "Acquire fence completion");
            if (sync.previousPresentSerial != 0)
            {
                // Reacquiring an image and waiting for its acquisition proves its previous
                // presentation finished. On this same presentation queue, that also retires
                // predecessors carried across swapchain recreation (KHR sample approach).
                ReleaseRetiredSwapchains();
                PresentSync& present = m_presentSync[sync.imageIndex];
                if (!m_hasPresentFences && present.serial == sync.previousPresentSerial)
                {
                    present.pending = false;
                }
            }
            CheckWSIResult(vkResetFences(device, 1, &sync.fence), "vkResetFences(acquire)");
            sync.pending               = false;
            sync.previousPresentSerial = 0;
        }
    }
}

void VulkanSwapchain::WaitForPresent(PresentSync& sync)
{
    if (sync.pending)
    {
        const VkDevice device = m_pDevice->GetVkHandle();
        const VkResult result = vkWaitForFences(device, 1, &sync.fence, VK_TRUE, UINT64_MAX);
        if (result == VK_ERROR_DEVICE_LOST)
        {
            GVulkanRHI->BlockSubmissions();
            sync.pending = false;
        }
        else
        {
            CheckWSIResult(result, "Presentation fence completion");
            CheckWSIResult(vkResetFences(device, 1, &sync.fence), "vkResetFences(present)");
            sync.pending = false;
        }
    }
}

void VulkanSwapchain::Destroy(VulkanSwapchainRecreateInfo* pRecreateInfo)
{
    const VkDevice device = m_pDevice->GetVkHandle();
    for (AcquireSync& sync : m_acquireSync)
    {
        CompleteAcquire(sync, true);
        if (sync.submissionSerial != 0 &&
            !m_pDevice->GetGfxQueue()->WaitForCompletion(sync.submissionSerial, UINT64_MAX))
        {
            // Distinguish device loss (destruction is legal) from an unproven wait.
            const VkResult result = vkDeviceWaitIdle(device);
            if (result != VK_ERROR_DEVICE_LOST)
            {
                CheckWSIResult(result, "Swapchain graphics completion");
            }
            else
            {
                GVulkanRHI->BlockSubmissions();
            }
        }
    }
    if (m_hasPresentFences)
    {
        for (PresentSync& sync : m_presentSync)
        {
            WaitForPresent(sync);
        }
    }
    else if (pRecreateInfo == nullptr && (m_swapchain || !m_retiredSwapchains.empty()))
    {
        // Unextended WSI has no host present-completion primitive at shutdown or
        // surface loss, when future reacquisition is impossible. Use the Vulkan
        // guide's device-idle teardown convention only at this terminal boundary.
        const VkResult result = vkDeviceWaitIdle(device);
        if (result != VK_ERROR_DEVICE_LOST)
        {
            CheckWSIResult(result, "Swapchain terminal idle wait");
        }
        else
        {
            GVulkanRHI->BlockSubmissions();
        }
    }

    if (GVulkanRHI->AreSubmissionsBlocked())
    {
        pRecreateInfo = nullptr;
    }
    VulkanRetiredSwapchain retired;
    retired.swapchain = m_swapchain;
    if (pRecreateInfo != nullptr)
    {
        retired.presentationSemaphores.reserve(m_presentSync.size());
        m_retiredSwapchains.reserve(m_retiredSwapchains.size() + 1);
    }
    for (PresentSync& sync : m_presentSync)
    {
        if (sync.pending && pRecreateInfo != nullptr)
        {
            retired.presentationSemaphores.push_back(sync.semaphore);
            sync.semaphore = nullptr;
        }
        else
        {
            m_pDevice->GetSemaphoreManager()->DestroySemaphore(sync.semaphore);
        }
        if (sync.fence)
        {
            vkDestroyFence(device, sync.fence, nullptr);
        }
    }
    for (AcquireSync& sync : m_acquireSync)
    {
        // An unused acquisition may still be signaled. Destroy it after its acquire
        // fence, rather than returning it to the pool of unsignaled semaphores.
        m_pDevice->GetSemaphoreManager()->DestroySemaphore(sync.semaphore);
        if (sync.fence)
        {
            vkDestroyFence(device, sync.fence, nullptr);
        }
    }
    if (pRecreateInfo != nullptr)
    {
        if (!retired.presentationSemaphores.empty())
        {
            m_retiredSwapchains.push_back(std::move(retired));
        }
        pRecreateInfo->swapchain         = m_swapchain;
        pRecreateInfo->surface           = m_surface;
        pRecreateInfo->retiredSwapchains = std::move(m_retiredSwapchains);
    }
    else
    {
        const bool retained = std::any_of(m_retiredSwapchains.begin(), m_retiredSwapchains.end(),
                                          [&](const VulkanRetiredSwapchain& previous) {
                                              return previous.swapchain == m_swapchain;
                                          });
        ReleaseRetiredSwapchains();
        if (m_swapchain != VK_NULL_HANDLE && !retained)
        {
            vkDestroySwapchainKHR(device, m_swapchain, nullptr);
        }
        VulkanPlatform::DestroySurface(GVulkanRHI->GetInstance(), m_surface);
    }
    m_acquireSync.clear();
    m_presentSync.clear();
    m_swapchainImages.clear();
    m_numImages      = 0;
    m_imageIndex     = -1;
    m_semaphoreIndex = -1;
    m_swapchain      = VK_NULL_HANDLE;
    m_surface        = VK_NULL_HANDLE;
}
} // namespace zen
