#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
#include "Utils/SharedPtr.h"

namespace zen
{
enum class RHIGPUTimingStatus : uint8_t
{
    eDisabled,
    ePending,
    eAvailable,
    eUnsupported,
    eDropped,
    eDiscarded,
    eError
};

inline const char* RHIGPUTimingStatusName(RHIGPUTimingStatus status)
{
    const char* name = "error";

    switch (status)
    {
        case RHIGPUTimingStatus::eDisabled: name = "disabled"; break;
        case RHIGPUTimingStatus::ePending: name = "pending"; break;
        case RHIGPUTimingStatus::eAvailable: name = "available"; break;
        case RHIGPUTimingStatus::eUnsupported: name = "unsupported"; break;
        case RHIGPUTimingStatus::eDropped: name = "dropped"; break;
        case RHIGPUTimingStatus::eDiscarded: name = "discarded"; break;
        case RHIGPUTimingStatus::eError: break;
    }

    return name;
}

struct RHIGPUTimestampInterval
{
    uint64_t begin{0};
    uint64_t end{0};
    uint32_t validBits{0};
    double periodNanoseconds{0};
};

// A scope must finish within one wrap of the queue's timestamp counter.
inline bool ConvertGPUTimestampsToMicroseconds(uint64_t begin,
                                               uint64_t end,
                                               uint32_t validBits,
                                               double periodNanoseconds,
                                               double& microseconds)
{
    const bool valid = validBits > 0 && validBits <= 64 && std::isfinite(periodNanoseconds) &&
        periodNanoseconds > 0;

    if (valid)
    {
        const uint64_t mask = validBits == 64 ? UINT64_MAX : (uint64_t(1) << validBits) - 1;

        microseconds = static_cast<double>((end - begin) & mask) * periodNanoseconds / 1000.0;
    }

    return valid && std::isfinite(microseconds);
}

// A recording owns this result until it is discarded or the native work completes.
// Publication is one-shot; readers need not call back into the RHI thread.
class RHIGPUTimingResult
{
public:
    bool Publish(RHIGPUTimingStatus status, double microseconds = 0)
    {
        return PublishValue(status, microseconds, {});
    }

    bool PublishTimestamps(const RHIGPUTimestampInterval& interval)
    {
        double microseconds = 0;

        const bool valid =
            ConvertGPUTimestampsToMicroseconds(interval.begin, interval.end, interval.validBits,
                                               interval.periodNanoseconds, microseconds);

        return PublishValue(valid ? RHIGPUTimingStatus::eAvailable : RHIGPUTimingStatus::eError,
                            microseconds, interval);
    }

    RHIGPUTimingStatus GetStatus() const
    {
        return m_status.load(std::memory_order_acquire);
    }

    double GetMicroseconds() const
    {
        return GetStatus() == RHIGPUTimingStatus::eAvailable ? m_microseconds : 0;
    }

    RHIGPUTimestampInterval GetTimestamps() const
    {
        return GetStatus() == RHIGPUTimingStatus::eAvailable ? m_interval :
                                                               RHIGPUTimestampInterval{};
    }

private:
    bool PublishValue(RHIGPUTimingStatus status,
                      double microseconds,
                      const RHIGPUTimestampInterval& interval)
    {
        const bool publish = status != RHIGPUTimingStatus::ePending &&
            !m_published.test_and_set(std::memory_order_acq_rel);

        if (publish)
        {
            if (status == RHIGPUTimingStatus::eAvailable &&
                (!std::isfinite(microseconds) || microseconds < 0))
            {
                status = RHIGPUTimingStatus::eError;
            }

            m_microseconds = status == RHIGPUTimingStatus::eAvailable ? microseconds : 0;

            m_interval =
                status == RHIGPUTimingStatus::eAvailable ? interval : RHIGPUTimestampInterval{};

            m_status.store(status, std::memory_order_release);
        }

        return publish;
    }

    std::atomic_flag m_published = ATOMIC_FLAG_INIT;
    std::atomic<RHIGPUTimingStatus> m_status{RHIGPUTimingStatus::ePending};
    double m_microseconds{0};
    RHIGPUTimestampInterval m_interval;
};

// Separate owners are released on both the recording and RHI completion threads.
using RHIGPUTimingPtr = SharedPtr<RHIGPUTimingResult, MultiThreadCounter>;

} // namespace zen
