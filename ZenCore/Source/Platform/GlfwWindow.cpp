#include "Platform/GlfwWindow.h"
#include "Common/Errors.h"
#include "Platform/InputController.h"

namespace zen::platform
{
static void GlfwErrorCallback(int code, const char* msg)
{
    LOGE("GLFW error [{}]: {}", code, msg);
}

GlfwWindowImpl::GlfwWindowImpl(const WindowConfig& config)
{
    // set window data
    m_data.width  = config.width;
    m_data.height = config.height;

    if (!glfwInit())
    {
        LOGE("Failed to initialize GLFW!");
        abort();
    }
    glfwSetErrorCallback(GlfwErrorCallback);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, config.resizable ? GLFW_TRUE : GLFW_FALSE);

    m_handle = glfwCreateWindow(config.width, config.height, config.title.c_str(), NULL, NULL);
    glfwSetWindowUserPointer(m_handle, (void*)this);
    CenterWindow();
    SetupWindowCallbacks();
}

GlfwWindowImpl::~GlfwWindowImpl()
{
    Destroy();
}

VkSurfaceKHR GlfwWindowImpl::CreateSurface(VkInstance instance) const
{
    ASSERT(instance != VK_NULL_HANDLE);
    VkSurfaceKHR surface;
    CHECK_VK_ERROR_AND_THROW(glfwCreateWindowSurface(instance, m_handle, nullptr, &surface),
                             "glfw create window surface");
    return surface;
}

std::vector<const char*> GlfwWindowImpl::GetInstanceExtensions()
{
    uint32_t count;
    const char** ext = glfwGetRequiredInstanceExtensions(&count);
    return {ext, ext + count};
}

void GlfwWindowImpl::Destroy()
{
    glfwDestroyWindow(m_handle);
    glfwTerminate();
}

bool GlfwWindowImpl::CenterWindow()
{
    int sx = 0, sy = 0;
    int px = 0, py = 0;
    int mx = 0, my = 0;
    int monitorCount = 0;
    int best_area    = 0;
    int final_x = 0, final_y = 0;

    glfwGetWindowSize(m_handle, &sx, &sy);
    glfwGetWindowPos(m_handle, &px, &py);

    // Iterate throug all monitors
    GLFWmonitor** m = glfwGetMonitors(&monitorCount);
    if (!m)
        return false;

    for (int j = 0; j < monitorCount; ++j)
    {

        glfwGetMonitorPos(m[j], &mx, &my);
        const GLFWvidmode* mode = glfwGetVideoMode(m[j]);
        if (!mode)
            continue;

        // Get intersection of two rectangles - screen and window
        int minX = std::max(mx, px);
        int minY = std::max(my, py);

        int maxX = std::min(mx + mode->width, px + sx);
        int maxY = std::min(my + mode->height, py + sy);

        // Calculate area of the intersection
        int area = std::max(maxX - minX, 0) * std::max(maxY - minY, 0);

        // If its bigger than actual (window covers more space on this monitor)
        if (area > best_area)
        {
            // Calculate proper position in this monitor
            final_x = mx + (mode->width - sx) / 2;
            final_y = my + (mode->height - sy) / 2;

            best_area = area;
        }
    }

    // We found something
    if (best_area)
        glfwSetWindowPos(m_handle, final_x, final_y);

    // Something is wrong - current window has NOT any intersection with any monitors. Move it to the default one.
    else
    {
        GLFWmonitor* primary = glfwGetPrimaryMonitor();
        if (primary)
        {
            const GLFWvidmode* desktop = glfwGetVideoMode(primary);

            if (desktop)
                glfwSetWindowPos(m_handle, (desktop->width - sx) / 2, (desktop->height - sy) / 2);
            else
                return false;
        }
        else
            return false;
    }

    return true;
}

void GlfwWindowImpl::SetupWindowCallbacks()
{
    const auto resize_callback = [](GLFWwindow* w, int width, int height) {
        GlfwWindowImpl* window      = static_cast<GlfwWindowImpl*>(glfwGetWindowUserPointer(w));
        window->m_data.width        = width;
        window->m_data.height       = height;
        window->m_data.shouldResize = true;

        if (window->m_onResize)
        {
            window->m_onResize(width, height);
        }
        LOGI("Window resized to {} x {}", width, height);
    };
    glfwSetWindowSizeCallback(m_handle, resize_callback);

    const auto key_callback = [](GLFWwindow* w, auto key, auto scancode, auto action, auto mode) {
        if (key < 0 || key > GLFW_KEY_LAST)
        {
            return;
        }
        switch (action)
        {
            case GLFW_PRESS: KeyboardMouseInput::GetInstance().PressKey(key); break;
            case GLFW_RELEASE: KeyboardMouseInput::GetInstance().ReleaseKey(key); break;
            default: break;
        }
    };
    glfwSetKeyCallback(m_handle, key_callback);

    const auto cursor_pos_callback = [](GLFWwindow* w, auto xPos, auto yPos) {
        KeyboardMouseInput::GetInstance().SetCursorPos(xPos, yPos);
    };
    glfwSetCursorPosCallback(m_handle, cursor_pos_callback);

    auto mouse_button_callback = [](GLFWwindow* window, int button, int action, int mods) {
        if (button < 0 || button > GLFW_MOUSE_BUTTON_LAST)
        {
            return;
        }
        switch (action)
        {
            case GLFW_PRESS:
            {
                KeyboardMouseInput::GetInstance().PressMouseButton(button);
                KeyboardMouseInput::GetInstance().SetMouseButtonRelease(button, false);
                break;
            }
            case GLFW_RELEASE:
            {
                KeyboardMouseInput::GetInstance().ReleaseMouseButton(button);
                KeyboardMouseInput::GetInstance().SetMouseButtonRelease(button, true);
                break;
            }
            default: break;
        }
    };
    glfwSetMouseButtonCallback(m_handle, mouse_button_callback);
}

void GlfwWindowImpl::ShowCursor() const
{
    glfwSetInputMode(m_handle, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
}

void GlfwWindowImpl::HideCursor() const
{
    glfwSetInputMode(m_handle, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
}

void GlfwWindowImpl::Update()
{
    glfwPollEvents();
    if (KeyboardMouseInput::GetInstance().WasKeyPressedOnce(GLFW_KEY_TAB))
    {
        m_data.showCursor = !m_data.showCursor;
        if (m_data.showCursor)
        {
            ShowCursor();
            KeyboardMouseInput::GetInstance().SetDirty(false);
            KeyboardMouseInput::GetInstance().Pause();
        }
        else
        {
            HideCursor();
            KeyboardMouseInput::GetInstance().SetDirty(true);
            KeyboardMouseInput::GetInstance().Resume();
        }
    }
    if (KeyboardMouseInput::GetInstance().IsKeyPressed(GLFW_KEY_ESCAPE) ||
        glfwWindowShouldClose(m_handle))
    {
        m_data.shouldClose = true;
        glfwSetWindowShouldClose(m_handle, GL_TRUE);
    }
}

} // namespace zen::platform