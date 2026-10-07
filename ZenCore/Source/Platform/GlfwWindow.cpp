// Migration fallback. Selected only with ZEN_WINDOW_BACKEND=GLFW.
#include "Platform/WindowBackend.h"
#include "Utils/Errors.h"
#include <algorithm>

namespace zen::platform
{
namespace
{
Key TranslateKey(int code)
{
    Key key = Key::Unknown;

    switch (code)
    {
#define ZEN_KEY(engine, sdl, glfw) \
    case glfw: key = Key::engine; break;
#include "KeyMapping.inl"
#undef ZEN_KEY
        default: break;
    }

    return key;
}

NativeWindow& FromHandle(GLFWwindow* handle)
{
    return *static_cast<NativeWindow*>(glfwGetWindowUserPointer(handle));
}

void OnResize(GLFWwindow* handle, int, int)
{
    WindowBackend::Resize(FromHandle(handle));
}

void OnClose(GLFWwindow* handle)
{
    WindowBackend::Close(FromHandle(handle));
}

void OnKey(GLFWwindow* handle, int key, int, int action, int)
{
    InputEvent event;

    event.type   = action == GLFW_RELEASE ? InputEventType::KeyUp : InputEventType::KeyDown;

    event.key    = TranslateKey(key);

    event.repeat = action == GLFW_REPEAT;

    WindowBackend::Deliver(FromHandle(handle), event);
}

void OnPosition(GLFWwindow* handle, double x, double y)
{
    InputEvent event;

    event.type = InputEventType::PointerMove;

    event.x    = float(x);

    event.y    = float(y);

    WindowBackend::Deliver(FromHandle(handle), event);
}

void OnButton(GLFWwindow* handle, int button, int action, int)
{
    if (button >= 0 && button <= GLFW_MOUSE_BUTTON_LAST)
    {
        InputEvent event;

        event.type   = action == GLFW_RELEASE ? InputEventType::ButtonUp : InputEventType::ButtonDown;

        event.button = static_cast<MouseButton>(button);

        WindowBackend::Deliver(FromHandle(handle), event);
    }
}

void OnScroll(GLFWwindow* handle, double x, double y)
{
    InputEvent event;

    event.type   = InputEventType::Wheel;

    event.deltaX = float(x);

    event.deltaY = float(y);

    WindowBackend::Deliver(FromHandle(handle), event);
}

void OnText(GLFWwindow* handle, unsigned int codepoint)
{
    InputEvent event;

    event.type = InputEventType::Text;

    if (codepoint < 0x80)
    {
        event.text.push_back(char(codepoint));
    }
    else
    {
        if (codepoint < 0x800)
        {
            event.text.push_back(char(0xC0 | (codepoint >> 6)));
        }
        else
        {
            if (codepoint < 0x10000)
            {
                event.text.push_back(char(0xE0 | (codepoint >> 12)));
            }
            else
            {
                event.text.push_back(char(0xF0 | (codepoint >> 18)));

                event.text.push_back(char(0x80 | ((codepoint >> 12) & 0x3F)));
            }

            event.text.push_back(char(0x80 | ((codepoint >> 6) & 0x3F)));
        }

        event.text.push_back(char(0x80 | (codepoint & 0x3F)));
    }

    WindowBackend::Deliver(FromHandle(handle), event);
}

void OnDrop(GLFWwindow* handle, int count, const char** paths)
{
    for (int index = 0; index < count; ++index)
    {
        InputEvent event;

        event.type = InputEventType::FileDrop;

        event.text = paths[index];

        WindowBackend::Deliver(FromHandle(handle), event);
    }
}

void OnFocus(GLFWwindow* handle, int focused)
{
    InputEvent event;

    event.type = focused ? InputEventType::FocusGained : InputEventType::FocusLost;

    WindowBackend::Deliver(FromHandle(handle), event);
}
} // namespace

GLFWwindow* WindowBackend::Borrow(const NativeWindow& window)
{
    window.CheckThreadOwnership();

    return static_cast<GLFWwindow*>(window.m_handle);
}

void WindowBackend::InstallCallbacks(NativeWindow& window)
{
    GLFWwindow* handle = Borrow(window);

    glfwSetWindowUserPointer(handle, &window);

    glfwSetFramebufferSizeCallback(handle, OnResize);

    glfwSetWindowCloseCallback(handle, OnClose);

    glfwSetKeyCallback(handle, OnKey);

    glfwSetCursorPosCallback(handle, OnPosition);

    glfwSetMouseButtonCallback(handle, OnButton);

    glfwSetWindowFocusCallback(handle, OnFocus);

    glfwSetCharCallback(handle, OnText);

    glfwSetScrollCallback(handle, OnScroll);

    glfwSetDropCallback(handle, OnDrop);
}

NativeWindow::NativeWindow(const WindowConfig& config) : m_resizable(config.resizable)
{
    WindowBackend::Acquire();

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    glfwWindowHint(GLFW_RESIZABLE, config.resizable);

    glfwWindowHint(GLFW_VISIBLE, config.visible);

    m_handle = glfwCreateWindow(int(config.width), int(config.height), config.title.c_str(), nullptr, nullptr);

    VERIFY_EXPR_MSG(m_handle != nullptr, "Cannot create GLFW fallback window");

    WindowBackend::InstallCallbacks(*this);

    WindowBackend::Register(*this);
}

NativeWindow::~NativeWindow()
{
    CheckThreadOwnership();

    SetCustomFrame(false);

    glfwDestroyWindow(WindowBackend::Borrow(*this));

    WindowBackend::Unregister(*this);
}

WindowExtent NativeWindow::GetExtent2D() const
{
    int width  = 0;

    int height = 0;

    glfwGetWindowSize(WindowBackend::Borrow(*this), &width, &height);

    return {uint32_t(std::max(width, 0)), uint32_t(std::max(height, 0))};
}

WindowExtent NativeWindow::GetFramebufferExtent() const
{
    int width  = 0;

    int height = 0;

    if (!IsMinimized())
    {
        glfwGetFramebufferSize(WindowBackend::Borrow(*this), &width, &height);
    }

    return {uint32_t(std::max(width, 0)), uint32_t(std::max(height, 0))};
}

float NativeWindow::GetDisplayScale() const
{
    float x = 1;

    float y = 1;

    glfwGetWindowContentScale(WindowBackend::Borrow(*this), &x, &y);

    return std::max(x, y);
}

float NativeWindow::GetUIScale() const
{
    const WindowExtent pixels = GetFramebufferExtent();

    const WindowExtent units  = GetExtent2D();

    return pixels.width > 0 ? GetDisplayScale() * float(units.width) / float(pixels.width) : 1.0f;
}

bool NativeWindow::SetCustomFrame(bool enabled)
{
    return WindowBackend::SetFallbackFrame(*this, enabled);
}

// The fallback frame exists only on Windows, where the application draws the controls.
WindowTitleBarLayout NativeWindow::GetTitleBarLayout() const
{
    WindowTitleBarLayout layout;

    layout.drawsControls = m_customFrame;

    return layout;
}

// GLFW has no file picker; callers provide their own path entry.
bool NativeWindow::SupportsFileDialogs()
{
    return false;
}

bool NativeWindow::ShowOpenFileDialog(const FileDialogFilter&, const std::string&)
{
    CheckThreadOwnership();

    return false;
}

bool NativeWindow::TakeFileDialogResult(std::string&)
{
    CheckThreadOwnership();

    return false;
}
bool NativeWindow::IsFocused() const
{
    return glfwGetWindowAttrib(WindowBackend::Borrow(*this), GLFW_FOCUSED) == GLFW_TRUE;
}

bool NativeWindow::IsMinimized() const
{
    return glfwGetWindowAttrib(WindowBackend::Borrow(*this), GLFW_ICONIFIED) == GLFW_TRUE;
}

bool NativeWindow::IsMaximized() const
{
    return glfwGetWindowAttrib(WindowBackend::Borrow(*this), GLFW_MAXIMIZED) == GLFW_TRUE;
}

bool NativeWindow::IsDecorated() const
{
    return glfwGetWindowAttrib(WindowBackend::Borrow(*this), GLFW_DECORATED) == GLFW_TRUE;
}

void NativeWindow::Show()
{
    glfwShowWindow(WindowBackend::Borrow(*this));

    WindowBackend::Resize(*this);
}

void NativeWindow::Hide()
{
    glfwHideWindow(WindowBackend::Borrow(*this));

    WindowBackend::Resize(*this);
}

void NativeWindow::Focus()
{
    glfwFocusWindow(WindowBackend::Borrow(*this));

    WindowBackend::Resize(*this);
}

void NativeWindow::Minimize()
{
    glfwIconifyWindow(WindowBackend::Borrow(*this));

    WindowBackend::Resize(*this);
}

void NativeWindow::Maximize()
{
    glfwMaximizeWindow(WindowBackend::Borrow(*this));

    WindowBackend::Resize(*this);
}

void NativeWindow::Restore()
{
    glfwRestoreWindow(WindowBackend::Borrow(*this));

    WindowBackend::Resize(*this);
}

void NativeWindow::SetSize(uint32_t width, uint32_t height)
{
    glfwSetWindowSize(WindowBackend::Borrow(*this), int(width), int(height));

    WindowBackend::Resize(*this);
}

void NativeWindow::SetMinimumSize(uint32_t width, uint32_t height)
{
    glfwSetWindowSizeLimits(WindowBackend::Borrow(*this), int(width), int(height), GLFW_DONT_CARE, GLFW_DONT_CARE);
}

WindowPosition NativeWindow::GetPosition() const
{
    WindowPosition position;

    glfwGetWindowPos(WindowBackend::Borrow(*this), &position.x, &position.y);

    return position;
}

void NativeWindow::SetPosition(WindowPosition position)
{
    glfwSetWindowPos(WindowBackend::Borrow(*this), position.x, position.y);
}

void NativeWindow::ShowCursor() const
{
    glfwSetInputMode(WindowBackend::Borrow(*this), GLFW_CURSOR, GLFW_CURSOR_NORMAL);

    m_cursorVisible = true;
}

void NativeWindow::HideCursor() const
{
    glfwSetInputMode(WindowBackend::Borrow(*this), GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    m_cursorVisible = false;
}
void NativeWindow::SetTextInputEnabled(bool)
{
    CheckThreadOwnership();
}

void NativeWindow::SetTextInputArea(int, int, int, int, int)
{
    CheckThreadOwnership();
}
} // namespace zen::platform
