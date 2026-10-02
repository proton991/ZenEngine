#include "Graphics/RHI/RHIGPUFrameTiming.h"
#include <algorithm>

namespace zen
{
namespace
{
bool UnwrapTimestamp(uint64_t ticks, uint64_t anchor, uint64_t mask, uint64_t halfRange, int64_t& offset)
{
    const uint64_t delta = (ticks - anchor) & mask;

    const bool valid     = delta != halfRange;

    if (valid)
    {
        offset = delta < halfRange ? static_cast<int64_t>(delta) : -static_cast<int64_t>((anchor - ticks) & mask);
    }

    return valid;
}
} // namespace

RHIGPUTimingPtr RHIGPUFrameTiming::AddInterval()
{
    RHIGPUTimingPtr result;

    if (!m_sealed.load(std::memory_order_acquire))
    {
        if (m_intervals.size() < kMaxIntervals)
        {
            result = MakeShared<RHIGPUTimingResult, MultiThreadCounter>();

            m_intervals.push_back(result);
        }
        else
        {
            m_recordingStatus = RHIGPUTimingStatus::eDropped;
        }
    }

    return result;
}

void RHIGPUFrameTiming::ExcludeInterval()
{
    if (!m_sealed.load(std::memory_order_acquire))
    {
        ++m_excludedIntervals;
    }
}

void RHIGPUFrameTiming::Seal(RHIGPUTimingStatus status)
{
    if (!m_sealed.load(std::memory_order_acquire))
    {
        if (status != RHIGPUTimingStatus::eAvailable)
        {
            m_recordingStatus = status == RHIGPUTimingStatus::ePending ? RHIGPUTimingStatus::eError : status;
        }

        m_sealed.store(true, std::memory_order_release);
    }
}

RHIGPUTimingStatus RHIGPUFrameTiming::Evaluate(double& microseconds) const
{
    RHIGPUTimingStatus status = RHIGPUTimingStatus::ePending;

    microseconds              = 0;

    if (m_sealed.load(std::memory_order_acquire))
    {
        status = m_recordingStatus;

        if (status == RHIGPUTimingStatus::eAvailable)
        {
            bool pending = false;

            for (const RHIGPUTimingPtr& interval : m_intervals)
            {
                const RHIGPUTimingStatus intervalStatus  = interval->GetStatus();

                pending                                 |= intervalStatus == RHIGPUTimingStatus::ePending;

                if (status == RHIGPUTimingStatus::eAvailable && intervalStatus != RHIGPUTimingStatus::eAvailable
                    && intervalStatus != RHIGPUTimingStatus::ePending)
                {
                    status = intervalStatus;
                }
            }

            if (pending)
            {
                status = RHIGPUTimingStatus::ePending;
            }
            else if (m_intervals.empty())
            {
                status = RHIGPUTimingStatus::eDiscarded;
            }
            else if (status == RHIGPUTimingStatus::eAvailable && !CalculateEnvelope(microseconds))
            {
                status = RHIGPUTimingStatus::eError;
            }
        }
    }

    return status;
}

bool RHIGPUFrameTiming::CalculateEnvelope(double& microseconds) const
{
    const RHIGPUTimestampInterval first = m_intervals.front()->GetTimestamps();

    uint32_t validBits                  = first.validBits;

    RHIGPUTimestampInterval anchor      = first;

    bool valid = validBits > 0 && validBits <= 64 && std::isfinite(first.periodNanoseconds) && first.periodNanoseconds > 0;

    for (const RHIGPUTimingPtr& interval : m_intervals)
    {
        const RHIGPUTimestampInterval ticks = interval->GetTimestamps();

        valid     &= ticks.validBits > 0 && ticks.validBits <= 64 && ticks.periodNanoseconds == first.periodNanoseconds;

        validBits  = std::min(validBits, ticks.validBits);

        if (ticks.validBits > anchor.validBits)
        {
            anchor = ticks;
        }
    }

    if (valid)
    {
        const uint64_t halfRange = uint64_t(1) << (validBits - 1);

        int64_t earliest         = 0;

        int64_t latest           = 0;

        for (const RHIGPUTimingPtr& interval : m_intervals)
        {
            const RHIGPUTimestampInterval ticks = interval->GetTimestamps();

            const uint64_t nativeMask           = ticks.validBits == 64 ? UINT64_MAX : (uint64_t(1) << ticks.validBits) - 1;

            const uint64_t duration             = (ticks.end - ticks.begin) & nativeMask;

            const uint64_t nativeHalfRange      = uint64_t(1) << (ticks.validBits - 1);

            int64_t begin                       = 0;

            int64_t end                         = 0;

            valid &= duration < halfRange && UnwrapTimestamp(ticks.begin, anchor.begin, nativeMask, nativeHalfRange, begin)
                  && UnwrapTimestamp(ticks.end, anchor.begin, nativeMask, nativeHalfRange, end) && end >= begin;

            earliest = std::min(earliest, begin);

            latest   = std::max(latest, end);
        }

        // Unsigned subtraction preserves the exact positive span even across signed zero.
        const uint64_t span  = static_cast<uint64_t>(latest) - static_cast<uint64_t>(earliest);

        valid               &= span < halfRange;

        if (valid)
        {
            microseconds = static_cast<double>(span) * first.periodNanoseconds / 1000.0;

            valid        = std::isfinite(microseconds);
        }
    }

    return valid;
}

RHIGPUTimingStatus RHIGPUFrameTiming::GetStatus() const
{
    double microseconds = 0;

    return Evaluate(microseconds);
}

double RHIGPUFrameTiming::GetMicroseconds() const
{
    double microseconds             = 0;

    const RHIGPUTimingStatus status = Evaluate(microseconds);

    return status == RHIGPUTimingStatus::eAvailable ? microseconds : 0;
}

size_t RHIGPUFrameTiming::GetIntervalCount() const
{
    return m_sealed.load(std::memory_order_acquire) ? m_intervals.size() : 0;
}

size_t RHIGPUFrameTiming::GetExcludedIntervalCount() const
{
    return m_sealed.load(std::memory_order_acquire) ? m_excludedIntervals : 0;
}
} // namespace zen
