#include "Platform/GlfwWindow.h"
#include "Utils/Errors.h"
#include "Platform/InputController.h"

namespace zen::platform
{
static void GlfwErrorCallback(int code, const char* pMsg)
{
    LOGE("GLFW error [{}]: {}", code, pMsg);
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

    m_pHandle = glfwCreateWindow(config.width, config.height, config.title.c_str(), NULL, NULL);
    glfwSetWindowUserPointer(m_pHandle, (void*)this);
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
    CHECK_VK_ERROR_AND_THROW(glfwCreateWindowSurface(instance, m_pHandle, nullptr, &surface),
                             "glfw create window surface");
    return surface;
}

HeapVector<const char*> GlfwWindowImpl::GetInstanceExtensions()
{
    uint32_t count;
    const char** ppExts = glfwGetRequiredInstanceExtensions(&count);
    HeapVector<const char*> result(count);
    for (uint32_t i = 0; i < count; i++)
    {
        result[i] = ppExts[i];
    }
    return result;
}

void GlfwWindowImpl::Destroy()
{
    glfwDestroyWindow(m_pHandle);
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

    glfwGetWindowSize(m_pHandle, &sx, &sy);
    glfwGetWindowPos(m_pHandle, &px, &py);

    // Iterate throug all monitors
    GLFWmonitor** ppMonitors = glfwGetMonitors(&monitorCount);
    if (!ppMonitors)
        return false;

    for (int j = 0; j < monitorCount; ++j)
    {

        glfwGetMonitorPos(ppMonitors[j], &mx, &my);
        const GLFWvidmode* pMode = glfwGetVideoMode(ppMonitors[j]);
        if (!pMode)
            continue;

        // Get intersection of two rectangles - screen and window
        int minX = std::max(mx, px);
        int minY = std::max(my, py);

        int maxX = std::min(mx + pMode->width, px + sx);
        int maxY = std::min(my + pMode->height, py + sy);

        // Calculate area of the intersection
        int area = std::max(maxX - minX, 0) * std::max(maxY - minY, 0);

        // If its bigger than actual (window covers more space on this monitor)
        if (area > best_area)
        {
            // Calculate proper position in this monitor
            final_x = mx + (pMode->width - sx) / 2;
            final_y = my + (pMode->height - sy) / 2;

            best_area = area;
        }
    }

    // We found something
    if (best_area)
        glfwSetWindowPos(m_pHandle, final_x, final_y);

    // Something is wrong - current window has NOT any intersection with any monitors. Move it to the default one.
    else
    {
        GLFWmonitor* pPrimary = glfwGetPrimaryMonitor();
        if (pPrimary)
        {
            const GLFWvidmode* pDesktop = glfwGetVideoMode(pPrimary);

            if (pDesktop)
                glfwSetWindowPos(m_pHandle, (pDesktop->width - sx) / 2, (pDesktop->height - sy) / 2);
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
    const auto resize_callback = [](GLFWwindow* pW, int width, int height) {
        GlfwWindowImpl* pWindow      = static_cast<GlfwWindowImpl*>(glfwGetWindowUserPointer(pW));
        pWindow->m_data.width        = width;
        pWindow->m_data.height       = height;
        pWindow->m_data.shouldResize = true;

        if (pWindow->m_onResize)
        {
            pWindow->m_onResize(width, height);
        }
        LOGI("Window resized to {} x {}", width, height);
    };
    glfwSetWindowSizeCallback(m_pHandle, resize_callback);

    const auto key_callback = [](GLFWwindow* pW, auto key, auto scancode, auto action, auto mode) {
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
    glfwSetKeyCallback(m_pHandle, key_callback);

    const auto cursor_pos_callback = [](GLFWwindow* pW, auto xPos, auto yPos) {
        KeyboardMouseInput::GetInstance().SetCursorPos(xPos, yPos);
    };
    glfwSetCursorPosCallback(m_pHandle, cursor_pos_callback);

    auto mouse_button_callback = [](GLFWwindow* pWindow, int button, int action, int mods) {
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
    glfwSetMouseButtonCallback(m_pHandle, mouse_button_callback);
}

void GlfwWindowImpl::ShowCursor() const
{
    glfwSetInputMode(m_pHandle, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
}

void GlfwWindowImpl::HideCursor() const
{
    glfwSetInputMode(m_pHandle, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
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
        glfwWindowShouldClose(m_pHandle))
    {
        m_data.shouldClose = true;
        glfwSetWindowShouldClose(m_pHandle, GL_TRUE);
    }
}

} // namespace zen::platform
