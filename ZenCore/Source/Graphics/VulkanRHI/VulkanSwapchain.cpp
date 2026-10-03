#include "Graphics/RHI/RHIFrameState.h"
#include "Graphics/RHI/RHIOptions.h"
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
bool CheckWSIResult(VkResult result, const char* operation, RHIError& error)
{
    const bool success = result == VK_SUCCESS;

    if (!success && !error.IsFailure())
    {
        error = MakeVulkanError(result, operation, __FILE__, __LINE__);

        if (!error.IsFailure())
        {
            error = {RHIErrorCode::eBackendFailure, int64_t(result), operation, __FILE__, __LINE__};
        }

        LOGE("{} failed: {}", operation, int32_t(result));
    }

    return success;
}

// A bounded enumeration tolerates changing surface counts without an endless retry.
template <typename T, typename Query> HeapVector<T> EnumerateWSI(Query query, const char* operation, RHIError& error)
{
    HeapVector<T> values;

    VkResult result = VK_INCOMPLETE;

    for (uint32_t attempt = 0; attempt < 4 && result == VK_INCOMPLETE && !error.IsFailure(); ++attempt)
    {
        uint32_t count = 0;

        if (CheckWSIResult(query(&count, nullptr), operation, error) && count != 0)
        {
            values.resize(count);

            result = query(&count, values.data());

            if (result == VK_SUCCESS)
            {
                values.resize(count);
            }
            else if (result != VK_INCOMPLETE)
            {
                CheckWSIResult(result, operation, error);
            }
        }
        else
        {
            result = VK_ERROR_INITIALIZATION_FAILED;
        }
    }

    if (result != VK_SUCCESS)
    {
        CheckWSIResult(result, operation, error);

        values.clear();
    }

    return values;
}

RHISurfaceOutcome SurfaceOutcome(VkResult result)
{
    RHISurfaceOutcome outcome = RHISurfaceOutcome::eFailed;

    switch (result)
    {
        case VK_SUCCESS: outcome = RHISurfaceOutcome::eReady; break;
        case VK_TIMEOUT:
        case VK_NOT_READY: outcome = RHISurfaceOutcome::eIncomplete; break;
        case VK_SUBOPTIMAL_KHR:
        case VK_ERROR_OUT_OF_DATE_KHR: outcome = RHISurfaceOutcome::eRecreate; break;
        case VK_ERROR_SURFACE_LOST_KHR: outcome = RHISurfaceOutcome::eSurfaceLost; break;
        default: break;
    }

    return outcome;
}

VkSurfaceFormatKHR ChooseSurfaceFormat(VkPhysicalDevice gpu, VkSurfaceKHR surface, RHIError& error)
{
    const HeapVector<VkSurfaceFormatKHR> formats = EnumerateWSI<VkSurfaceFormatKHR>(
        [=](uint32_t* count, VkSurfaceFormatKHR* values) {
            return vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, count, values);
        },
        "vkGetPhysicalDeviceSurfaceFormatsKHR", error);

    VkSurfaceFormatKHR selected{};

    bool found = false;

    // The renderer applies gamma itself, so prefer UNORM to avoid applying it twice.
    for (VkFormat preferred :
         {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB})
    {
        for (const VkSurfaceFormatKHR& format : formats)
        {
            if (format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
                && (format.format == preferred || format.format == VK_FORMAT_UNDEFINED))
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
        CheckWSIResult(VK_ERROR_FORMAT_NOT_SUPPORTED, "Surface RGBA8 presentation format", error);
    }

    return selected;
}

VkPresentModeKHR ChoosePresentMode(VkPhysicalDevice gpu,
                                   VkSurfaceKHR     surface,
                                   bool             vsync,
                                   RHIPresentMode   request,
                                   RHIError&        error)
{
    const HeapVector<VkPresentModeKHR> modes = EnumerateWSI<VkPresentModeKHR>(
        [=](uint32_t* count, VkPresentModeKHR* values) {
            return vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, count, values);
        },
        "vkGetPhysicalDeviceSurfacePresentModesKHR", error);

    // Every request ends with FIFO, the only mode the specification guarantees.
    VkPresentModeKHR priority[] = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_FIFO_KHR};

    if (request == RHIPresentMode::eFifoRelaxed)
    {
        priority[0] = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    }
    else if (request == RHIPresentMode::eMailbox)
    {
        priority[0] = VK_PRESENT_MODE_MAILBOX_KHR;
    }
    else if (request == RHIPresentMode::eImmediate)
    {
        priority[0] = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    else if (request == RHIPresentMode::eDefault && !vsync)
    {
        priority[0] = VK_PRESENT_MODE_IMMEDIATE_KHR;

        priority[1] = VK_PRESENT_MODE_MAILBOX_KHR;
    }

    VkPresentModeKHR selected = VK_PRESENT_MODE_FIFO_KHR;

    bool found                = false;

    for (VkPresentModeKHR mode : priority)
    {
        if (!found && std::find(modes.begin(), modes.end(), mode) != modes.end())
        {
            selected = mode;

            found    = true;
        }
    }

    if (!found)
    {
        CheckWSIResult(VK_ERROR_INITIALIZATION_FAILED, "Surface presentation mode", error);
    }

    if (request != RHIPresentMode::eDefault && selected != priority[0])
    {
        LOGW("Requested present mode {} is not supported by the surface; using FIFO", static_cast<int32_t>(priority[0]));
    }

    return selected;
}
} // namespace

static VkCompositeAlphaFlagBitsKHR ChooseCompositeAlpha(VkCompositeAlphaFlagBitsKHR request,
                                                        VkCompositeAlphaFlagsKHR    supported,
                                                        RHIError&                   error)
{
    VkCompositeAlphaFlagBitsKHR selected = request;

    if ((request & supported) == 0)
    {
        selected                                                           = VK_COMPOSITE_ALPHA_FLAG_BITS_MAX_ENUM_KHR;

        static constexpr VkCompositeAlphaFlagBitsKHR compositeAlphaFlags[] = {
            VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};

        for (VkCompositeAlphaFlagBitsKHR compositeAlpha : compositeAlphaFlags)
        {
            if ((compositeAlpha & supported) != 0)
            {
                LOGW("(Swapchain) Composite alpha '{}' not supported. Selecting '{}.", VkToString(request),
                     VkToString(compositeAlpha));

                selected = compositeAlpha;

                break;
            }
        }

        if (selected == VK_COMPOSITE_ALPHA_FLAG_BITS_MAX_ENUM_KHR)
        {
            CheckWSIResult(VK_ERROR_INITIALIZATION_FAILED, "Surface composite alpha", error);
        }
    }

    return selected;
}

void VulkanSwapchain::DestroyOldSwapchain(VkSwapchainKHR& oldSwapchain)
{
    if (oldSwapchain != VK_NULL_HANDLE)
    {
        const bool retained =
            std::any_of(m_retiredSwapchains.begin(), m_retiredSwapchains.end(),
                        [oldSwapchain](const VulkanRetiredSwapchain& retired) { return retired.swapchain == oldSwapchain; });

        if (!retained)
        {
            vkDestroySwapchainKHR(m_pDevice->GetVkHandle(), oldSwapchain, nullptr);
        }

        oldSwapchain = VK_NULL_HANDLE;
    }
}

VulkanSwapchain::VulkanSwapchain(uint32_t                     width,
                                 uint32_t                     height,
                                 bool                         enableVSync,
                                 VulkanSwapchainRecreateInfo* pRecreateInfo) :
    m_pDevice(GVulkanRHI->GetDevice())
{
    VkDevice device             = m_pDevice->GetVkHandle();

    VkPhysicalDevice gpu        = GVulkanRHI->GetPhysicalDevice();

    m_hasPresentFences          = m_pDevice->GetExtensionFlags().hasSwapchainMaintenance1;

    VkSwapchainKHR oldSwapchain = VK_NULL_HANDLE;

    if (pRecreateInfo != nullptr)
    {
        m_surface           = pRecreateInfo->surface;

        oldSwapchain        = pRecreateInfo->swapchain;

        m_retiredSwapchains = std::move(pRecreateInfo->retiredSwapchains);

        *pRecreateInfo      = {};
    }

    do
    {
        VERIFY_EXPR_MSG(m_surface != VK_NULL_HANDLE, "Swapchain requires a native surface");

        VkBool32 canPresent = VK_FALSE;

        if (!CheckWSIResult(
                vkGetPhysicalDeviceSurfaceSupportKHR(gpu, m_pDevice->GetGfxQueue()->GetFamilyIndex(), m_surface, &canPresent),
                "vkGetPhysicalDeviceSurfaceSupportKHR", m_error))
        {
            break;
        }

        if (!canPresent)
        {
            CheckWSIResult(VK_ERROR_FEATURE_NOT_PRESENT, "Graphics queue presentation support", m_error);

            break;
        }

        VkSurfaceCapabilitiesKHR capabilities{};

        if (!CheckWSIResult(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, m_surface, &capabilities),
                            "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", m_error))
        {
            break;
        }

        VkExtent2D extent = capabilities.currentExtent;

        if (extent.width == UINT32_MAX)
        {
            extent.width  = std::clamp(width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);

            extent.height = std::clamp(height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
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
            uint32_t imageCount = std::max(capabilities.minImageCount, GRHIFrameState.GetNumFramesInFlight());

            if (capabilities.maxImageCount != 0)
            {
                imageCount = std::min(imageCount, capabilities.maxImageCount);
            }

            if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
            {
                CheckWSIResult(VK_ERROR_FEATURE_NOT_PRESENT, "Surface transfer-destination support", m_error);

                break;
            }

            const VkSurfaceFormatKHR format = ChooseSurfaceFormat(gpu, m_surface, m_error);

            m_format                        = format.format;

            m_colorSpace                    = format.colorSpace;

            m_presentMode = ChoosePresentMode(gpu, m_surface, enableVSync, RHIOptions::GetInstance().PresentMode(), m_error);

            VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};

            info.surface          = m_surface;

            info.minImageCount    = imageCount;

            info.imageExtent      = extent;

            info.imageArrayLayers = 1;

            info.preTransform     = (capabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                                      ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                      : capabilities.currentTransform;

            info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;

            info.imageFormat      = m_format;

            info.imageColorSpace  = m_colorSpace;

            info.imageUsage =
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);

            info.presentMode = m_presentMode;

            info.compositeAlpha =
                ChooseCompositeAlpha(VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, capabilities.supportedCompositeAlpha, m_error);

            info.clipped      = VK_TRUE;

            info.oldSwapchain = oldSwapchain;

            if (m_error.IsFailure())
            {
                break;
            }

            if (!CheckWSIResult(vkCreateSwapchainKHR(device, &info, nullptr, &m_swapchain), "vkCreateSwapchainKHR", m_error))
            {
                m_swapchain = VK_NULL_HANDLE;

                break;
            }

            DestroyOldSwapchain(oldSwapchain);

            m_swapchainImages = EnumerateWSI<VkImage>(
                [=, this](uint32_t* count, VkImage* images) {
                    return vkGetSwapchainImagesKHR(device, m_swapchain, count, images);
                },
                "vkGetSwapchainImagesKHR", m_error);

            if (m_error.IsFailure())
            {
                break;
            }

            m_numImages = static_cast<uint32_t>(m_swapchainImages.size());

            m_acquireSync.resize(m_numImages);

            m_presentSync.resize(m_numImages);

            VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};

            for (uint32_t i = 0; i < m_numImages; ++i)
            {
                AcquireSync& acquire = m_acquireSync[i];

                acquire.semaphore    = m_pDevice->GetSemaphoreManager()->GetOrCreateSemaphore();

                if (acquire.semaphore == nullptr)
                {
                    m_error = m_pDevice->GetSemaphoreManager()->GetLastError();

                    break;
                }

                acquire.semaphore->SetDebugName(NameID(fmt::format("ImageAcquired-{}", i)));

                if (!CheckWSIResult(vkCreateFence(device, &fenceInfo, nullptr, &acquire.fence), "vkCreateFence(acquire)",
                                    m_error))
                {
                    acquire.fence = VK_NULL_HANDLE;

                    break;
                }

                PresentSync& present = m_presentSync[i];

                present.semaphore    = m_pDevice->GetSemaphoreManager()->GetOrCreateSemaphore();

                if (present.semaphore == nullptr)
                {
                    m_error = m_pDevice->GetSemaphoreManager()->GetLastError();

                    break;
                }

                present.semaphore->SetDebugName(NameID(fmt::format("RenderComplete-{}", i)));

                if (m_hasPresentFences)
                {
                    if (!CheckWSIResult(vkCreateFence(device, &fenceInfo, nullptr, &present.fence), "vkCreateFence(present)",
                                        m_error))
                    {
                        present.fence = VK_NULL_HANDLE;

                        break;
                    }
                }
            }

            if (!m_error.IsFailure())
            {
                LOGI("Swapchain: {} images, {}x{}, format {}, present mode {}", m_numImages, extent.width, extent.height,
                     VkToString(format), VkToString(m_presentMode));
            }
        }

    } while (false);

    if (m_error.IsFailure())
    {
        DestroyOldSwapchain(oldSwapchain);

        Destroy(nullptr);
    }
}

int32_t VulkanSwapchain::AcquireNextImage(VulkanSemaphore** outSemaphore)
{
    return AcquireNextImageChecked(outSemaphore).imageIndex;
}

RHIAcquireResult VulkanSwapchain::AcquireNextImageChecked(VulkanSemaphore** outSemaphore)
{
    VERIFY_EXPR_MSG(m_imageIndex < 0, "Swapchain already has an acquired image");

    VERIFY_EXPR(outSemaphore != nullptr);

    *outSemaphore   = nullptr;

    m_acquireResult = {};

    m_lastResult    = VK_NOT_READY;

    if (GVulkanRHI->AreSubmissionsBlocked())
    {
        m_acquireResult = {RHISurfaceOutcome::eFailed, -1, GVulkanRHI->GetTerminalError()};
    }
    else if (m_numImages != 0)
    {
        const int32_t nextSemaphore = (m_semaphoreIndex + 1) % static_cast<int32_t>(m_numImages);

        for (AcquireSync& sync : m_acquireSync)
        {
            CompleteAcquire(sync, false);
        }

        AcquireSync& acquire = m_acquireSync[nextSemaphore];

        CompleteAcquire(acquire, true);

        const bool completed = !acquire.pending && !GVulkanRHI->AreSubmissionsBlocked()
                            && (acquire.submissionSerial == 0
                                || m_pDevice->GetGfxQueue()->WaitForCompletion(acquire.submissionSerial, UINT64_MAX));

        if (completed)
        {
            acquire.submissionSerial = 0;

            uint32_t index           = UINT32_MAX;

            m_lastResult             = vkAcquireNextImageKHR(m_pDevice->GetVkHandle(), m_swapchain, 1000000000ull,
                                                             acquire.semaphore->GetVkHandle(), acquire.fence, &index);

            m_acquireResult.outcome  = SurfaceOutcome(m_lastResult);

            if (m_lastResult == VK_SUCCESS || m_lastResult == VK_SUBOPTIMAL_KHR)
            {
                VERIFY_EXPR_MSG_F(index < m_numImages, "Acquired image {} exceeds swapchain image count {}", index,
                                  m_numImages);

                acquire.pending               = true;

                m_acquiredSuboptimal          = m_lastResult == VK_SUBOPTIMAL_KHR;

                m_semaphoreIndex              = nextSemaphore;

                m_imageIndex                  = static_cast<int32_t>(index);

                acquire.imageIndex            = index;

                acquire.previousPresentSerial = m_presentSync[index].pending ? m_presentSync[index].serial : 0;

                if (m_hasPresentFences)
                {
                    WaitForPresent(m_presentSync[index]);
                }

                if (!GVulkanRHI->AreSubmissionsBlocked())
                {
                    *outSemaphore              = acquire.semaphore;

                    m_acquireResult.imageIndex = m_imageIndex;
                }
            }
            else if (m_acquireResult.outcome == RHISurfaceOutcome::eFailed)
            {
                m_acquireResult.error = MakeVulkanError(m_lastResult, "vkAcquireNextImageKHR", __FILE__, __LINE__);

                // The frame owner decides whether a rejected acquisition is terminal.
            }
        }

        if (GVulkanRHI->AreSubmissionsBlocked())
        {
            m_acquireResult = {RHISurfaceOutcome::eFailed, -1, GVulkanRHI->GetTerminalError()};
        }
    }

    return m_acquireResult;
}

void VulkanSwapchain::MarkAcquireSemaphoreSubmitted(uint64_t submissionSerial)
{
    if (m_imageIndex < 0 || submissionSerial == 0)
    {
        VERIFY_EXPR_MSG_F(false, "Cannot mark an acquire semaphore without an accepted submission");
    }

    m_acquireSync[m_semaphoreIndex].submissionSerial = submissionSerial;
}

bool VulkanSwapchain::Present(VulkanSemaphore* pRenderingCompleteSemaphore)
{
    if (m_imageIndex < 0 || GVulkanRHI->AreSubmissionsBlocked())
    {
        VERIFY_EXPR_MSG_F(false, "Cannot present without an acquired image and a usable device");
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
    sync.pending = m_lastResult == VK_SUCCESS || m_lastResult == VK_SUBOPTIMAL_KHR || m_lastResult == VK_ERROR_OUT_OF_DATE_KHR
                || m_lastResult == VK_ERROR_SURFACE_LOST_KHR;

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

    m_presentResult      = {SurfaceOutcome(m_lastResult),
                       sync.pending ? RHIPresentAcceptance::eEnqueued
                            : (m_lastResult == VK_ERROR_OUT_OF_HOST_MEMORY || m_lastResult == VK_ERROR_OUT_OF_DEVICE_MEMORY)
                                ? RHIPresentAcceptance::eRejected
                                : RHIPresentAcceptance::eUncertain,
                            {},
                            m_lastResult == VK_SUCCESS || m_lastResult == VK_SUBOPTIMAL_KHR};

    if (m_presentResult.outcome == RHISurfaceOutcome::eFailed)
    {
        m_presentResult.error = MakeVulkanError(m_lastResult, "vkQueuePresentKHR", __FILE__, __LINE__);

        GVulkanRHI->BlockSubmissions(m_presentResult.error);
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

        const VkResult result =
            wait ? vkWaitForFences(device, 1, &sync.fence, VK_TRUE, UINT64_MAX) : vkGetFenceStatus(device, sync.fence);

        if (result == VK_ERROR_DEVICE_LOST)
        {
            GVulkanRHI->BlockSubmissions(MakeVulkanError(result, "Swapchain completion", __FILE__, __LINE__));

            sync.pending = false;

            ReportVulkanDeviceLoss(result, "acquire fence");
        }
        else if (result != VK_NOT_READY && result != VK_TIMEOUT)
        {
            VERIFY_EXPR_MSG_F(result == VK_SUCCESS, "Acquire fence completion failed: {}", int32_t(result));

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

            VERIFY_EXPR_MSG(vkResetFences(device, 1, &sync.fence) == VK_SUCCESS, "vkResetFences(acquire) failed");

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
            GVulkanRHI->BlockSubmissions(MakeVulkanError(result, "Swapchain completion", __FILE__, __LINE__));

            sync.pending = false;

            ReportVulkanDeviceLoss(result, "present fence");
        }
        else
        {
            VERIFY_EXPR_MSG_F(result == VK_SUCCESS, "Presentation fence completion failed: {}", int32_t(result));

            VERIFY_EXPR_MSG(vkResetFences(device, 1, &sync.fence) == VK_SUCCESS, "vkResetFences(present) failed");

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

        if (sync.submissionSerial != 0 && !m_pDevice->GetGfxQueue()->WaitForCompletion(sync.submissionSerial, UINT64_MAX))
        {
            // Distinguish device loss (destruction is legal) from an unproven wait.
            const VkResult result = vkDeviceWaitIdle(device);

            if (result != VK_ERROR_DEVICE_LOST)
            {
                VERIFY_EXPR_MSG_F(result == VK_SUCCESS, "Swapchain graphics completion failed: {}", int32_t(result));
            }
            else
            {
                GVulkanRHI->BlockSubmissions(MakeVulkanError(result, "Swapchain completion", __FILE__, __LINE__));
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
            VERIFY_EXPR_MSG_F(result == VK_SUCCESS, "Swapchain terminal idle wait failed: {}", int32_t(result));
        }
        else
        {
            GVulkanRHI->BlockSubmissions(MakeVulkanError(result, "Swapchain completion", __FILE__, __LINE__));
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
        const bool retained =
            std::any_of(m_retiredSwapchains.begin(), m_retiredSwapchains.end(),
                        [&](const VulkanRetiredSwapchain& previous) { return previous.swapchain == m_swapchain; });

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
