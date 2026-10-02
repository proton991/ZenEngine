#pragma once

#include "Platform/GlfwWindow.h"
#include "Utils/Errors.h"
#include <chrono>

namespace zen
{
// A native window lifecycle probe: keep rendering while covered, then spend the
// same interval minimized. The frame counter must advance after both restores.
class SceneRendererWindowTest
{
public:
    SceneRendererWindowTest(GLFWwindow* window, uint32_t seconds) : m_window(window), m_seconds(seconds) {}

    ~SceneRendererWindowTest()
    {
        if (m_cover != nullptr)
        {
            glfwDestroyWindow(m_cover);
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

            if (m_phase == Phase::eForeground && m_cycles == 1 && (frame == m_phaseFrame + 2 || frame == m_phaseFrame + 3))
            {
                // Exercise focus loss from captured-camera mode through the installed
                // engine/ImGui key callback chain, without coupling this probe to ZenUI.
                GLFWkeyfun keyCallback = glfwSetKeyCallback(m_window, nullptr);

                glfwSetKeyCallback(m_window, keyCallback);

                if (keyCallback != nullptr)
                {
                    keyCallback(m_window, GLFW_KEY_F1, 0, frame == m_phaseFrame + 2 ? GLFW_PRESS : GLFW_RELEASE, 0);
                }
            }

            if (m_phase == Phase::eForeground && frame >= m_phaseFrame + 8)
            {
                int width  = 0;
                int height = 0;
                int x      = 0;
                int y      = 0;

                glfwGetWindowSize(m_window, &width, &height);

                glfwGetWindowPos(m_window, &x, &y);

                m_cover =
                    glfwCreateWindow(width + 80, height + 80, "Background rendering test - covering demo", nullptr, nullptr);

                if (m_cover == nullptr)
                {
                    LOG_ERROR_AND_THROW("Cannot create the background test cover window");
                }

                glfwSetWindowPos(m_cover, x - 40, y - 40);

                glfwFocusWindow(m_cover);

                StartPhase(Phase::eCovered, frame, now);
            }
            else if (m_phase == Phase::eCovered && elapsed >= m_seconds)
            {
                glfwDestroyWindow(m_cover);

                m_cover = nullptr;

                glfwFocusWindow(m_window);

                StartPhase(Phase::eReturned, frame, now);
            }
            else if (m_phase == Phase::eReturned && frame >= m_phaseFrame + 8)
            {
                if (VerifyForeground(frame, now))
                {
                    glfwIconifyWindow(m_window);

                    StartPhase(Phase::eMinimized, frame, now);
                }
            }
            else if (m_phase == Phase::eMinimized && elapsed >= m_seconds)
            {
                glfwRestoreWindow(m_window);

                glfwFocusWindow(m_window);

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
                     glfwGetWindowAttrib(m_window, GLFW_FOCUSED), glfwGetWindowAttrib(m_window, GLFW_ICONIFIED));

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
        m_succeeded = glfwGetWindowAttrib(m_window, GLFW_FOCUSED) == GLFW_TRUE
                   && glfwGetWindowAttrib(m_window, GLFW_ICONIFIED) == GLFW_FALSE;

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

    GLFWwindow*                           m_window;
    GLFWwindow*                           m_cover{nullptr};
    uint32_t                              m_seconds;
    uint32_t                              m_phaseFrame{0};
    uint32_t                              m_cycles{0};
    bool                                  m_succeeded{true};
    Phase                                 m_phase{Phase::eForeground};
    std::chrono::steady_clock::time_point m_started{std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point m_lastProgress{m_started};
};
} // namespace zen
