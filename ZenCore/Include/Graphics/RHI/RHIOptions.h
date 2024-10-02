#pragma once
#include <cstdint>

namespace zen::rhi
{
class RHIOptions
{
public:
    // Deleted to prevent copying and assignment
    RHIOptions(const RHIOptions&)            = delete;
    RHIOptions& operator=(const RHIOptions&) = delete;

    // Static method to get the single instance of the class
    static RHIOptions& GetInstance()
    {
        static RHIOptions instance;
        return instance;
    }

    void LoadDefault()
    {
        m_VkRHIOptions.uploadCmdBufferSemaphore = false;
        m_VkRHIOptions.maxDescriptorSetPerPool  = 64;
        m_VkRHIOptions.reuseSwapchainOnResize   = false;
    }

    bool VKUploadCmdBufferSemaphore() const
    {
        return m_VkRHIOptions.uploadCmdBufferSemaphore;
    }

    bool WaitForFrameCompletion() const
    {
        return true;
    }

    uint32_t MaxDescriptorSetPerPool() const
    {
        return m_VkRHIOptions.maxDescriptorSetPerPool;
    }

    bool ReuseSwapChainOnResize() const
    {
        return m_VkRHIOptions.reuseSwapchainOnResize;
    }

private:
    // Private constructor to prevent instantiation
    RHIOptions()
    {
        LoadDefault();
    }

    struct VulkanRHIOptions
    {
        bool uploadCmdBufferSemaphore;
        uint32_t maxDescriptorSetPerPool;
        bool reuseSwapchainOnResize;
    } m_VkRHIOptions;
};
} // namespace zen::rhi