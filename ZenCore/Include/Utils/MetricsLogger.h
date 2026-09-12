#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <utility>

namespace zen
{
// Accumulates microseconds once, either explicitly or when the scope ends.
class ScopedMetricsTimer
{
public:
    ScopedMetricsTimer(bool enabled, double& elapsed) : m_pElapsed(enabled ? &elapsed : nullptr)
    {
        if (enabled)
        {
            m_start = std::chrono::steady_clock::now();
        }
    }

    ~ScopedMetricsTimer()
    {
        Stop();
    }

    void Stop()
    {
        if (m_pElapsed != nullptr)
        {
            *m_pElapsed += std::chrono::duration<double, std::micro>(
                               std::chrono::steady_clock::now() - m_start)
                               .count();
            m_pElapsed = nullptr;
        }
    }

    ScopedMetricsTimer(const ScopedMetricsTimer&) = delete;

    ScopedMetricsTimer& operator=(const ScopedMetricsTimer&) = delete;

private:
    double* m_pElapsed;
    std::chrono::steady_clock::time_point m_start{};
};

struct MetricsLogOptions
{
    bool enabled{true};
    uint32_t sampleEvery{120};
    std::chrono::milliseconds minInterval{5000};
};

// One instance per reporting stream, owned by the calling thread. No global registry,
// locks, formatting, or allocation on rejected samples. The sink can log, persist,
// or inspect a subsystem's own structured sample; it must copy data it retains.
template <typename Sample> class MetricsLogger
{
public:
    using Clock = std::chrono::steady_clock;
    using Sink  = std::function<void(const Sample&)>;

    void Configure(MetricsLogOptions options)
    {
        options.sampleEvery = std::max(1u, options.sampleEvery);
        options.minInterval = std::max(options.minInterval, std::chrono::milliseconds::zero());
        m_options           = options;
        m_remaining         = 0;
        m_hasSample         = false;
        m_requested         = false;
    }

    const MetricsLogOptions& GetOptions() const
    {
        return m_options;
    }

    void SetSink(Sink sink)
    {
        m_sink = std::move(sink);
    }

    void RequestSample()
    {
        m_requested = true;
    }

    bool TryBeginSample()
    {
        return Eligible() && BeginAt(Clock::now());
    }

    // Explicit time also permits deterministic testing without sleeping.
    bool TryBeginSample(Clock::time_point now)
    {
        return Eligible() && BeginAt(now);
    }

    void Publish(const Sample& sample) const
    {
        if (m_options.enabled && m_sink)
        {
            m_sink(sample);
        }
    }

private:
    bool Eligible()
    {
        bool result{};

        if (m_options.enabled)
        {
            if (m_requested)
            {
                result = true;
            }
            else if (m_remaining != 0)
            {
                --m_remaining;
                result = false;
            }
            else
            {
                m_remaining = m_options.sampleEvery - 1;
                result      = true;
            }
        }

        return result;
    }

    bool BeginAt(Clock::time_point now)
    {
        bool result{};

        if (!(!m_requested && m_hasSample && now - m_lastSample < m_options.minInterval))
        {
            m_lastSample = now;
            m_hasSample  = true;
            m_requested  = false;
            m_remaining  = m_options.sampleEvery - 1;
            result       = true;
        }

        return result;
    }

    MetricsLogOptions m_options;
    Sink m_sink;
    Clock::time_point m_lastSample{};
    uint32_t m_remaining{0};
    bool m_hasSample{false};
    bool m_requested{false};
};
} // namespace zen
