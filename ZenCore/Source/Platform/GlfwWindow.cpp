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
    CheckThreadOwnership();
    ASSERT(instance != VK_NULL_HANDLE);
    VkSurfaceKHR surface;
    CHECK_VK_ERROR_AND_THROW(glfwCreateWindowSurface(instance, m_pHandle, nullptr, &surface),
                             "glfw create window surface");
    return surface;
}

void GlfwWindowImpl::CheckThreadOwnership() const
{
    ASSERT(std::this_thread::get_id() == m_ownerThread);
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
    {
        return false;
    }

    for (int j = 0; j < monitorCount; ++j)
    {

        glfwGetMonitorPos(ppMonitors[j], &mx, &my);
        const GLFWvidmode* pMode = glfwGetVideoMode(ppMonitors[j]);
        if (!pMode)
        {
            continue;
        }

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
    {
        glfwSetWindowPos(m_pHandle, final_x, final_y);
    }

    // Something is wrong - current window has NOT any intersection with any monitors. Move it to the default one.
    else
    {
        GLFWmonitor* pPrimary = glfwGetPrimaryMonitor();
        if (pPrimary)
        {
            const GLFWvidmode* pDesktop = glfwGetVideoMode(pPrimary);

            if (pDesktop)
            {
                glfwSetWindowPos(m_pHandle, (pDesktop->width - sx) / 2,
                                 (pDesktop->height - sy) / 2);
            }
            else
            {
                return false;
            }
        }
        else
        {
            return false;
        }
    }

    return true;
}

void GlfwWindowImpl::OnWindowSize(GLFWwindow* handle, int width, int height)
{
    GlfwWindowImpl* window = static_cast<GlfwWindowImpl*>(glfwGetWindowUserPointer(handle));
    window->m_data.width   = width;
    window->m_data.height  = height;
    // WSI can send WM_SIZE while RenderCore waits for RHI. Publish the dimensions
    // now, but defer application callbacks so they cannot re-enter that RHI wait.
    window->m_data.shouldResize = true;
}

static void OnKey(GLFWwindow*, int key, int, int action, int)
{
    if (key >= 0 && key <= GLFW_KEY_LAST)
    {
        switch (action)
        {
            case GLFW_PRESS: KeyboardMouseInput::GetInstance().PressKey(key); break;
            case GLFW_RELEASE: KeyboardMouseInput::GetInstance().ReleaseKey(key); break;
            default: break;
        }
    }
}

static void OnCursorPosition(GLFWwindow*, double x, double y)
{
    KeyboardMouseInput::GetInstance().SetCursorPos(x, y);
}

static void OnMouseButton(GLFWwindow*, int button, int action, int)
{
    if (button >= 0 && button <= GLFW_MOUSE_BUTTON_LAST)
    {
        KeyboardMouseInput& input = KeyboardMouseInput::GetInstance();
        if (action == GLFW_PRESS)
        {
            input.PressMouseButton(button);
            input.SetMouseButtonRelease(button, false);
        }
        else if (action == GLFW_RELEASE)
        {
            input.ReleaseMouseButton(button);
            input.SetMouseButtonRelease(button, true);
        }
    }
}

void GlfwWindowImpl::SetupWindowCallbacks()
{
    glfwSetWindowSizeCallback(m_pHandle, &GlfwWindowImpl::OnWindowSize);
    glfwSetKeyCallback(m_pHandle, OnKey);
    glfwSetCursorPosCallback(m_pHandle, OnCursorPosition);
    glfwSetMouseButtonCallback(m_pHandle, OnMouseButton);
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
    if (m_data.shouldResize)
    {
        const uint32_t width  = static_cast<uint32_t>(m_data.width);
        const uint32_t height = static_cast<uint32_t>(m_data.height);
        // Clear first: a new notification during the callback belongs to the next update.
        m_data.shouldResize = false;
        if (m_onResize)
        {
            m_onResize(width, height);
        }
        LOGI("Window resized to {} x {}", width, height);
    }
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
