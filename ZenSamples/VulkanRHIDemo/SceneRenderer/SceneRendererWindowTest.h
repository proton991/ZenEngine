#pragma once

#include "Platform/NativeWindow.h"
#include "Utils/Errors.h"
#include <chrono>

namespace zen
{
// A native window lifecycle probe: keep rendering while covered, then spend the
// same interval minimized. The frame counter must advance after both restores.
class SceneRendererWindowTest
{
public:
    SceneRendererWindowTest(platform::NativeWindow& window, uint32_t seconds) : m_window(&window), m_seconds(seconds) {}

    ~SceneRendererWindowTest()
    {
        if (m_cover != nullptr)
        {
            delete m_cover;
        }
    }

    bool Enabled() const
    {
        return m_seconds != 0;
    }

    bool Complete() const
    {
        return m_phase == Phase::eComplete;
    }

    bool Succeeded() const
    {
        return Complete() && m_succeeded;
    }

    void Update(uint32_t frame)
    {
        if (Enabled() && !Complete())
        {
            const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

            const double elapsed                            = std::chrono::duration<double>(now - m_started).count();

            if (m_phase == Phase::eForeground && frame >= m_phaseFrame + 8)
            {
                const platform::WindowExtent extent     = m_window->GetExtent2D();

                const platform::WindowPosition position = m_window->GetPosition();

                m_cover                                 = new platform::NativeWindow(
                    {"Background rendering test - covering demo", false, extent.width + 80, extent.height + 80});

                m_cover->SetPosition({position.x - 40, position.y - 40});

                m_cover->Focus();

                StartPhase(Phase::eCovered, frame, now);
            }
            else if (m_phase == Phase::eCovered && elapsed >= m_seconds)
            {
                delete m_cover;

                m_cover = nullptr;

                m_window->Focus();

                StartPhase(Phase::eReturned, frame, now);
            }
            else if (m_phase == Phase::eReturned && frame >= m_phaseFrame + 8)
            {
                if (VerifyForeground(frame, now))
                {
                    m_window->Minimize();

                    StartPhase(Phase::eMinimized, frame, now);
                }
            }
            else if (m_phase == Phase::eMinimized && elapsed >= m_seconds)
            {
                m_window->Restore();

                m_window->Focus();

                StartPhase(Phase::eRestored, frame, now);
            }
            else if (m_phase == Phase::eRestored && frame >= m_phaseFrame + 8)
            {
                if (VerifyForeground(frame, now))
                {
                    ++m_cycles;

                    StartPhase(m_cycles == 3 ? Phase::eComplete : Phase::eForeground, frame, now);
                }
            }

            if (now - m_lastProgress >= std::chrono::seconds(5))
            {
                LOGI("Window test progress: phase={} frame={} focused={} iconified={}", PhaseName(m_phase), frame,
                     m_window->IsFocused(), m_window->IsMinimized());

                m_lastProgress = now;
            }
        }
    }

private:
    enum class Phase
    {
        eForeground,
        eCovered,
        eReturned,
        eMinimized,
        eRestored,
        eComplete
    };

    static const char* PhaseName(Phase phase)
    {
        const char* name = "complete";

        switch (phase)
        {
            case Phase::eForeground: name = "foreground"; break;
            case Phase::eCovered: name = "covered"; break;
            case Phase::eReturned: name = "returned"; break;
            case Phase::eMinimized: name = "minimized"; break;
            case Phase::eRestored: name = "restored"; break;
            case Phase::eComplete: break;
        }

        return name;
    }

    bool VerifyForeground(uint32_t frame, std::chrono::steady_clock::time_point now)
    {
        m_succeeded = m_window->IsFocused() && !m_window->IsMinimized();

        if (!m_succeeded)
        {
            LOGE("Window test did not regain focus after {}", PhaseName(m_phase));

            StartPhase(Phase::eComplete, frame, now);
        }

        return m_succeeded;
    }

    void StartPhase(Phase phase, uint32_t frame, std::chrono::steady_clock::time_point now)
    {
        m_phase      = phase;

        m_phaseFrame = frame;

        m_started    = now;

        LOGI("Window test transition: phase={} cycle={} frame={}", PhaseName(phase), m_cycles, frame);
    }

    platform::NativeWindow*               m_window;
    platform::NativeWindow*               m_cover{nullptr};
    uint32_t                              m_seconds;
    uint32_t                              m_phaseFrame{0};
    uint32_t                              m_cycles{0};
    bool                                  m_succeeded{true};
    Phase                                 m_phase{Phase::eForeground};
    std::chrono::steady_clock::time_point m_started{std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point m_lastProgress{m_started};
};
} // namespace zen
