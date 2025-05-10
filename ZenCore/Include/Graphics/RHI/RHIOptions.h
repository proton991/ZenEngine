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
        m_VkRHIOptions.useDynamicRendering      = true;
        m_VkRHIOptions.uploadCmdBufferSemaphore = false;
        m_VkRHIOptions.maxDescriptorSetPerPool  = 64;
    }

    bool UseDynamicRendering() const
    {
        return m_VkRHIOptions.useDynamicRendering;
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

private:
    // Private constructor to prevent instantiation
    RHIOptions()
    {
        LoadDefault();
    }

    struct VulkanRHIOptions
    {
        bool uploadCmdBufferSemaphore;
        bool useDynamicRendering;
        uint32_t maxDescriptorSetPerPool;
    } m_VkRHIOptions;
};
} // namespace zen::rhi