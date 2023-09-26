#pragma once
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
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

    [[nodiscard]] auto ShouldClose() const { return m_data.should_close; }

    void ShowCursor() const;

    void HideCursor() const;

    VkExtent2D GetExtent2D() const
    {
        return {static_cast<uint32_t>(m_data.width), static_cast<uint32_t>(m_data.height)};
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
        bool should_close{false};
        bool show_cursor{true};
        bool should_resize{false};
    } m_data;
};

} // namespace zen::platform