#pragma once
#include "RHIGPUTiming.h"
#include "Templates/HeapVector.h"

namespace zen
{
// One frame's native command-buffer intervals in a backend-guaranteed common clock.
// Add/Exclude/Seal run on one producer (the RHI worker); readers never touch the
// mutable vector until Seal publishes it. Tokens resolve independently at retirement.
class RHIGPUFrameTiming
{
public:
    static constexpr size_t kMaxIntervals = 256;

    RHIGPUTimingPtr AddInterval();

    void ExcludeInterval();

    void Seal(RHIGPUTimingStatus status = RHIGPUTimingStatus::eAvailable);

    RHIGPUTimingStatus GetStatus() const;

    double GetMicroseconds() const;

    size_t GetIntervalCount() const;

    size_t GetExcludedIntervalCount() const;

private:
    RHIGPUTimingStatus Evaluate(double& microseconds) const;

    bool CalculateEnvelope(double& microseconds) const;

    HeapVector<RHIGPUTimingPtr> m_intervals;
    size_t m_excludedIntervals{0};
    RHIGPUTimingStatus m_recordingStatus{RHIGPUTimingStatus::eAvailable};
    std::atomic<bool> m_sealed{false};
};

using RHIGPUFrameTimingPtr = SharedPtr<RHIGPUFrameTiming, MultiThreadCounter>;
} // namespace zen
