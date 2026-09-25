#pragma once
#include <cstdint>

namespace zen
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

    // Startup capability switch used by non-RT conformance runs. Set before device creation.
    void SetRayTracingEnabled(bool enabled)
    {
        m_rayTracingEnabled = enabled;
    }
    bool RayTracingEnabled() const
    {
        return m_rayTracingEnabled;
    }

    void SetGPUProfilerMarkers(bool enabled)
    {
        m_gpuProfilerMarkers = enabled;
    }
    bool GPUProfilerMarkers() const
    {
        return m_gpuProfilerMarkers;
    }
    void SetGPUMemoryStats(bool enabled)
    {
        m_gpuMemoryStats = enabled;
    }
    bool GPUMemoryStats() const
    {
        return m_gpuMemoryStats;
    }
    void SetValidationEnabled(bool enabled)
    {
        m_validationEnabled = enabled;
    }
    bool ValidationEnabled() const
    {
        return m_validationEnabled;
    }

private:
    bool m_rayTracingEnabled{true};
    bool m_gpuProfilerMarkers{false};
    bool m_gpuMemoryStats{false};
    bool m_validationEnabled{true};
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
} // namespace zen
