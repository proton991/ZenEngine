#pragma once
#include <chrono>

namespace zen::platform
{
/**
 * @brief Encapsulates basic usage of chrono, providing a means to calculate float
 *        durations between time points via function calls.
 */
class Timer
{
public:
    using Seconds      = std::ratio<1>;
    using Milliseconds = std::ratio<1, 1000>;
    using Microseconds = std::ratio<1, 1000000>;
    using Nanoseconds  = std::ratio<1, 1000000000>;

    // Configure
    using Clock             = std::chrono::steady_clock;
    using DefaultResolution = Seconds;

    Timer() : m_startTime(Clock::now()), m_previousTick(Clock::now()) {}

    virtual ~Timer() = default;

    /**
	 * @brief Starts the timer, elapsed() now returns the duration since start()
	 */
    void Start()
    {
        if (!m_running)
        {
            m_running   = true;
            m_startTime = Clock::now();
        }
    }

    /**
	 * @brief Laps the timer, elapsed() now returns the duration since the last lap()
	 */
    void Lap()
    {
        m_lapping = true;
        m_lapTime = Clock::now();
    }

    /**
	 * @brief Stops the timer, elapsed() now returns 0
	 * @return The total execution time between `start()` and `stop()`
	 */
    template <typename T = DefaultResolution> double Stop()
    {
        if (!m_running)
        {
            return 0;
        }

        m_running     = false;
        m_lapping     = false;
        auto duration = std::chrono::duration<double, T>(Clock::now() - m_startTime);
        m_startTime   = Clock::now();
        m_lapTime     = Clock::now();

        return duration.count();
    }

    /**
	 * @brief Calculates the time difference between now and when the timer was started
	 *        if lap() was called, then between now and when the timer was last lapped
	 * @return The duration between the two time points (default in seconds)
	 */
    template <typename T = DefaultResolution> double Elapsed()
    {
        if (!m_running)
        {
            return 0;
        }

        Clock::time_point start = m_startTime;

        if (m_lapping)
        {
            start = m_lapTime;
        }

        return std::chrono::duration<double, T>(Clock::now() - start).count();
    }

    /**
	 * @brief Calculates the time difference between now and the last time this function was called
	 * @return The duration between the two time points (default in seconds)
	 */
    template <typename T = DefaultResolution> double Tick()
    {
        auto now       = Clock::now();
        auto duration  = std::chrono::duration<double, T>(now - m_previousTick);
        m_previousTick = now;
        return duration.count();
    }

    /**
	 * @brief Check if the timer is running
	 */
    bool IsRunning() const
    {
        return m_running;
    }

private:
    bool m_running{false};

    bool m_lapping{false};

    Clock::time_point m_startTime;

    Clock::time_point m_lapTime;

    Clock::time_point m_previousTick;
};
} // namespace zen::platform