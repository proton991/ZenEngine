#pragma once

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
    }

    bool VKUploadCmdBufferSemaphore() const
    {
        return m_VkRHIOptions.uploadCmdBufferSemaphore;
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
    } m_VkRHIOptions;
};
} // namespace zen::rhi