#pragma once
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <functional>
#include "NativeWindow.h"

namespace zen::platform
{

class GlfwWindowImpl : public NativeWindow
{
public:
    GlfwWindowImpl(const WindowConfig& config);
    ~GlfwWindowImpl();

    VkSurfaceKHR CreateSurface(VkInstance instance) const override;

    std::vector<const char*> GetInstanceExtensions() override;

    void Update();

    [[nodiscard]] auto ShouldClose() const
    {
        return m_data.shouldClose;
    }

    void ShowCursor() const;

    void HideCursor() const;

    VkExtent2D GetExtent2D() const
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
        return m_handle;
    }

private:
    void SetupWindowCallbacks();
    bool CenterWindow();
    void Destroy();

    GLFWwindow* m_handle;
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