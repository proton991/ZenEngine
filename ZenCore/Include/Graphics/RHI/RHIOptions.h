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

    void SetDebugPrintfEnabled(bool enabled)
    {
        m_debugPrintfEnabled = enabled;
    }

    bool DebugPrintfEnabled() const
    {
        return m_debugPrintfEnabled;
    }

    void SetExecutionCountersEnabled(bool enabled)
    {
        m_executionCountersEnabled = enabled;
    }

    bool ExecutionCountersEnabled() const
    {
        return m_executionCountersEnabled;
    }

    void SetRobustBufferAccessEnabled(bool enabled)
    {
        m_robustBufferAccessEnabled = enabled;
    }

    bool RobustBufferAccessEnabled() const
    {
        return m_robustBufferAccessEnabled;
    }

private:
    bool m_executionCountersEnabled{true};
    bool m_debugPrintfEnabled{false};
    bool m_robustBufferAccessEnabled{false};
    bool m_rayTracingEnabled{true};
    bool m_gpuProfilerMarkers{false};
    bool m_gpuMemoryStats{false};
    bool m_validationEnabled{true};
    RHIOptions() = default;
};
} // namespace zen
