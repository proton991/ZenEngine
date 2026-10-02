#pragma once
#include <cstdint>
#include <string_view>

namespace zen
{
// Swapchain present-mode request. eDefault follows the viewport's VSync flag: FIFO when VSync
// is on; IMMEDIATE, then MAILBOX, then FIFO when it is off. An explicit mode falls back to
// FIFO, the only mode every surface supports.
enum class RHIPresentMode : uint8_t
{
    eDefault,
    eFifo,
    eFifoRelaxed,
    eMailbox,
    eImmediate
};

// Accepts "default", "fifo", "fifo_relaxed", "mailbox" and "immediate".
inline bool ParseRHIPresentMode(std::string_view name, RHIPresentMode& outMode)
{
    bool valid = true;

    if (name == "default")
    {
        outMode = RHIPresentMode::eDefault;
    }
    else if (name == "fifo")
    {
        outMode = RHIPresentMode::eFifo;
    }
    else if (name == "fifo_relaxed")
    {
        outMode = RHIPresentMode::eFifoRelaxed;
    }
    else if (name == "mailbox")
    {
        outMode = RHIPresentMode::eMailbox;
    }
    else if (name == "immediate")
    {
        outMode = RHIPresentMode::eImmediate;
    }
    else
    {
        valid = false;
    }

    return valid;
}

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

    void SetDeviceLossDiagnostics(bool enabled)
    {
        m_deviceLossDiagnostics = enabled;
    }

    bool DeviceLossDiagnostics() const
    {
        return m_deviceLossDiagnostics;
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

    // Read when a swapchain is created or recreated.
    void SetPresentMode(RHIPresentMode mode)
    {
        m_presentMode = mode;
    }

    RHIPresentMode PresentMode() const
    {
        return m_presentMode;
    }

    // RHI resources or command contexts that outlive backend teardown are ownership bugs.
    // Strict checks abort through the verification path; otherwise teardown logs and continues,
    // so a leak does not turn a shipping build's exit into a crash. Debug builds default to strict.
    void SetStrictTeardownChecks(bool enabled)
    {
        m_strictTeardownChecks = enabled;
    }

    bool StrictTeardownChecks() const
    {
        return m_strictTeardownChecks;
    }

private:
    RHIPresentMode m_presentMode{RHIPresentMode::eDefault};
#if defined(NDEBUG)
    bool m_strictTeardownChecks{false};
#else
    bool m_strictTeardownChecks{true};
#endif
    bool m_executionCountersEnabled{true};
    bool m_debugPrintfEnabled{false};
    bool m_robustBufferAccessEnabled{false};
    bool m_rayTracingEnabled{true};
    bool m_gpuProfilerMarkers{false};
    bool m_gpuMemoryStats{false};
    bool m_deviceLossDiagnostics{false};
    bool m_validationEnabled{true};
    RHIOptions() = default;
};
} // namespace zen
