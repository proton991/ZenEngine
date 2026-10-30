#pragma once
#include <algorithm>
#include <chrono>

namespace zen::runtime
{
// Scene time is independent of presentation/UI time. Call Advance only for a
// scene update, and ResetWallTime after startup, restore or other deliberate waits.
class SceneClock
{
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    void ResetWallTime(TimePoint now)
    {
        m_previous    = now;
        m_initialized = true;
    }

    void SetPaused(bool paused, TimePoint now)
    {
        if (m_paused != paused)
        {
            m_paused = paused;
            ResetWallTime(now);
        }
    }

    float Advance(TimePoint now, bool fixedStep = false)
    {
        float seconds = 0.0f;
        if (m_initialized && !m_paused)
        {
            seconds = fixedStep ? 1.0f / 60.0f : std::clamp(std::chrono::duration<float>(now - m_previous).count(), 0.0f, 0.1f);
            m_elapsed += seconds;
        }
        ResetWallTime(now);
        return seconds;
    }

    double GetElapsed() const
    {
        return m_elapsed;
    }

    bool IsPaused() const
    {
        return m_paused;
    }

private:
    TimePoint m_previous;
    double    m_elapsed{0};
    bool      m_initialized{false};
    bool      m_paused{false};
};
} // namespace zen::runtime
