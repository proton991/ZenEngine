#pragma once
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <functional>
#include <thread>
#include "NativeWindow.h"
#include "Templates/HeapVector.h"

namespace zen::platform
{

class GlfwWindowImpl : public NativeWindow
{
public:
    GlfwWindowImpl(const WindowConfig& config);

    ~GlfwWindowImpl();

    void CheckThreadOwnership() const;

    // UI applications route shortcuts after their UI frame has resolved capture.
    void Update(bool processInputShortcuts = true);

    [[nodiscard]] bool ShouldClose() const
    {
        return m_data.shouldClose;
    }

    void ShowCursor() const;

    void HideCursor() const;

    WindowExtent GetExtent2D() const override
    {
        return {static_cast<uint32_t>(m_data.width), static_cast<uint32_t>(m_data.height)};
    }

    float GetAspect() override
    {
        return static_cast<float>(m_data.width) / static_cast<float>(m_data.height);
    }

    void SetOnResize(std::function<void(uint32_t, uint32_t)> callback)
    {
        m_onResize = std::move(callback);
    }

    GLFWwindow* GetHandle() const
    {
        return m_pHandle;
    }

private:
    static void OnWindowSize(GLFWwindow* handle, int width, int height);

    void SetupWindowCallbacks();

    bool CenterWindow();

    void Destroy();

    GLFWwindow*           m_pHandle;
    const std::thread::id m_ownerThread{std::this_thread::get_id()};
    struct WindowData
    {
        // size
        int width;
        int height;

        // status
        bool shouldClose{false};
        bool showCursor{true};
        bool shouldResize{false};
    } m_data;

    std::function<void(uint32_t, uint32_t)> m_onResize;
};

} // namespace zen::platform
