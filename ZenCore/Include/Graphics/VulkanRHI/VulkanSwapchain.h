#pragma once
#include "Templates/HeapVector.h"
#include "Graphics/VulkanRHI/VulkanHeaders.h"


namespace zen
{
class VulkanRHI;
class VulkanDevice;
class VulkanSemaphore;
struct VulkanTexture;

struct VulkanSwapchainRecreateInfo
{
    VkSwapchainKHR swapchain{VK_NULL_HANDLE};
    VkSurfaceKHR surface{VK_NULL_HANDLE};
};

class VulkanSwapchain
{
public:
    VulkanSwapchain(void* pWindowPtr,
                    uint32_t width,
                    uint32_t height,
                    bool enableVSync,
                    VulkanSwapchainRecreateInfo* pRecreateInfo);

    VkSwapchainKHR GetVkHandle() const
    {
        return m_swaphchain;
    }

    VkFormat GetFormat() const
    {
        return m_format;
    }

    const uint32_t& GetNumSwapchainImages() const
    {
        return m_numImages;
    }

    const VkImage* GetSwapchainImages() const
    {
        return m_swapchainImages.data();
    }

    VkExtent2D GetExtent() const
    {
        return {m_internalWidth, m_internalHeight};
    }
    VkResult GetLastResult() const
    {
        return m_lastResult;
    }
    bool NeedsRecreation() const
    {
        return m_lastResult == VK_SUBOPTIMAL_KHR || m_lastResult == VK_ERROR_OUT_OF_DATE_KHR ||
            m_lastResult == VK_ERROR_SURFACE_LOST_KHR;
    }

    int32_t AcquireNextImage(VulkanSemaphore** pOutSemaphore);

    bool Present(VulkanSemaphore* pRenderingCompleteSemaphore);

    void MarkAcquireSemaphoreSubmitted(uint64_t submissionSerial);

    void Destroy(VulkanSwapchainRecreateInfo* pRecreateInfo);

private:
    VulkanDevice* m_pDevice{nullptr};
    VkSwapchainKHR m_swaphchain{VK_NULL_HANDLE};
    VkSurfaceKHR m_surface{VK_NULL_HANDLE};
    uint32_t m_internalWidth{0};
    uint32_t m_internalHeight{0};
    VkFormat m_format{VK_FORMAT_UNDEFINED};
    VkColorSpaceKHR m_colorSpace{VK_COLORSPACE_SRGB_NONLINEAR_KHR};
    VkPresentModeKHR m_presentMode{VK_PRESENT_MODE_IMMEDIATE_KHR};
    uint32_t m_numImages{0};

    HeapVector<VkImage> m_swapchainImages;
    int32_t m_imageIndex{-1};
    int32_t m_semaphoreIndex{-1};
    HeapVector<VulkanSemaphore*> m_imageAcquiredSemaphores;
    HeapVector<uint64_t> m_imageAcquiredSemaphoreSubmissionSerials;
    VkResult m_lastResult{VK_SUCCESS};
    bool m_acquiredSuboptimal{false};
};
} // namespace zen
