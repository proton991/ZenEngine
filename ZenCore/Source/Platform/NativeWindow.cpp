#include "Platform/NativeWindow.h"
#include "Platform/InputController.h"
#include "Platform/WindowBackend.h"
#include "Templates/HeapVector.h"
#include "Utils/Errors.h"
#include <algorithm>
#include <cmath>

namespace zen::platform
{
namespace
{
// Process event ownership belongs to the application, not individual windows.
struct WindowApplication
{
    HeapVector<NativeWindow*> windows;
    std::thread::id           owner;
    uint32_t                  references{0};
};

WindowApplication application;
} // namespace

bool WindowBackend::IsInitialized()
{
    return application.references != 0;
}

void WindowBackend::Acquire()
{
    if (application.references == 0)
    {
        application.owner = std::this_thread::get_id();

#if defined(ZEN_WINDOW_SDL3)
        SDL_SetMainReady();

        VERIFY_EXPR_MSG(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
#else
        VERIFY_EXPR_MSG(glfwInit() == GLFW_TRUE, "Cannot initialize GLFW fallback");
#endif
    }

    ASSERT(application.owner == std::this_thread::get_id());

    ++application.references;
}

void WindowBackend::Register(NativeWindow& window)
{
    application.windows.push_back(&window);
}

void WindowBackend::Unregister(NativeWindow& window)
{
    window.CheckThreadOwnership();

    application.windows.erase(std::find(application.windows.begin(), application.windows.end(), &window));

    --application.references;

    if (application.references == 0)
    {
#if defined(ZEN_WINDOW_SDL3)
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
#else
        glfwTerminate();
#endif
        KeyboardMouseInput::GetInstance().Reset();

        // Release registry storage before the engine allocator's shutdown report.
        application.windows = HeapVector<NativeWindow*>();
    }
}

#if defined(ZEN_WINDOW_SDL3)
void WindowBackend::Dispatch(const SDL_Event& event)
{
    for (NativeWindow* window : application.windows)
    {
        if (window->m_rawObserver)
        {
            window->m_rawObserver(window->m_observerContext, &event);
        }

        if (event.type == SDL_EVENT_QUIT)
        {
            Close(*window);
        }
        else if (SDL_GetWindowFromEvent(&event) == Borrow(*window))
        {
            DispatchToWindow(*window, event);
        }
    }
}
#endif

void WindowBackend::Observe(NativeWindow& window, void (*callback)(void*, const void*), void* context)
{
    window.CheckThreadOwnership();

    ASSERT(callback == nullptr || window.m_rawObserver == nullptr);

    window.m_rawObserver     = callback;

    window.m_observerContext = context;
}

void WindowBackend::Resize(NativeWindow& window)
{
    // May run inside a native message dispatch while the RHI thread is working.
    // Never call application or renderer callbacks from the native event handler.
    window.m_resizePending = true;
}

void WindowBackend::Close(NativeWindow& window)
{
    window.m_shouldClose = true;
}

void WindowBackend::DispatchResizes()
{
    for (NativeWindow* window : application.windows)
    {
        if (window->m_resizePending)
        {
            window->m_resizePending   = false;

            const WindowExtent extent = window->GetFramebufferExtent();

            if (window->m_onResize)
            {
                window->m_onResize(extent.width, extent.height);
            }
        }
    }
}

void WindowBackend::Deliver(NativeWindow& window, const InputEvent& event)
{
    KeyboardMouseInput& input = KeyboardMouseInput::GetInstance();

    switch (event.type)
    {
        case InputEventType::KeyDown:
            if (!event.repeat && event.key != Key::Unknown)
            {
                input.PressKey(event.key);
            }
            break;
        case InputEventType::KeyUp: input.ReleaseKey(event.key); break;
        case InputEventType::ButtonDown:
            input.PressMouseButton(event.button);

            input.SetMouseButtonRelease(event.button, false);
            break;
        case InputEventType::ButtonUp:
            input.ReleaseMouseButton(event.button);

            input.SetMouseButtonRelease(event.button, true);
            break;
        case InputEventType::PointerMove:
#if defined(ZEN_WINDOW_SDL3)
            if (window.m_cursorVisible)
            {
                window.m_cursorX = event.x;

                window.m_cursorY = event.y;
            }
            else
            {
                window.m_cursorX += event.deltaX;

                window.m_cursorY += event.deltaY;
            }

#else
            window.m_cursorX = event.x;

            window.m_cursorY = event.y;
#endif
            input.SetCursorPos(window.m_cursorX, window.m_cursorY);
            break;
        case InputEventType::FocusLost:
            input.Reset();

            window.ShowCursor();
            break;
        default: break;
    }

    if (window.m_onInput)
    {
        window.m_onInput(event);
    }
}

void NativeWindow::CheckThreadOwnership() const
{
    ASSERT(std::this_thread::get_id() == m_ownerThread);
}

void NativeWindow::PollEvents()
{
    ASSERT(application.references != 0 && application.owner == std::this_thread::get_id());

#if defined(ZEN_WINDOW_SDL3)
    SDL_Event event;

    while (SDL_PollEvent(&event))
    {
        WindowBackend::Dispatch(event);
    }
#else
    glfwPollEvents();
#endif
    WindowBackend::DispatchResizes();
}

void NativeWindow::WaitEvents(double seconds)
{
    ASSERT(application.references != 0 && application.owner == std::this_thread::get_id());

#if defined(ZEN_WINDOW_SDL3)
    SDL_Event event;

    if (SDL_WaitEventTimeout(&event, int(std::clamp(seconds, 0.0, 60.0) * 1000)))
    {
        WindowBackend::Dispatch(event);
    }

    PollEvents();
#else
    glfwWaitEventsTimeout(seconds);

    WindowBackend::DispatchResizes();
#endif
}

void NativeWindow::Update(bool processInputShortcuts)
{
    CheckThreadOwnership();

    PollEvents();

    KeyboardMouseInput& input = KeyboardMouseInput::GetInstance();

    if (processInputShortcuts && input.WasKeyPressedOnce(Key::Tab))
    {
        if (m_cursorVisible)
        {
            HideCursor();

            input.SetDirty(true);

            input.Resume();
        }
        else
        {
            ShowCursor();

            input.SetDirty(false);

            input.Pause();
        }
    }

    if (processInputShortcuts && input.IsKeyPressed(Key::Escape))
    {
        RequestClose();
    }
}

float NativeWindow::GetAspect() const
{
    const WindowExtent size = GetFramebufferExtent();

    return size.height != 0 ? float(size.width) / float(size.height) : 1.0f;
}

bool NativeWindow::ShouldClose() const
{
    CheckThreadOwnership();

    return m_shouldClose;
}

void NativeWindow::RequestClose()
{
    CheckThreadOwnership();

    m_shouldClose = true;
}

void NativeWindow::SetOnResize(std::function<void(uint32_t, uint32_t)> callback)
{
    CheckThreadOwnership();

    m_onResize = std::move(callback);
}

void NativeWindow::SetOnInput(std::function<void(const InputEvent&)> callback)
{
    CheckThreadOwnership();

    m_onInput = std::move(callback);
}

void NativeWindow::SetTitleBarRegion(const WindowTitleBarRegion& region)
{
    CheckThreadOwnership();

    m_titleBar = region;
}

WindowHit NativeWindow::HitTest(float x, float y) const
{
    const WindowExtent extent = GetExtent2D();

    WindowHit hit             = WindowHit::Client;

    // A frame that stays decorated, as on macOS, resizes from its native edges.
    if (m_customFrame && m_resizable && !IsDecorated() && !IsMaximized())
    {
        const float border = 6.0f * GetUIScale();

        const bool left    = x < border;

        const bool right   = x >= float(extent.width) - border;

        const bool top     = y < border;

        const bool bottom  = y >= float(extent.height) - border;

        if (top)
        {
            hit = left ? WindowHit::TopLeft : right ? WindowHit::TopRight : WindowHit::Top;
        }
        else if (bottom)
        {
            hit = left ? WindowHit::BottomLeft : right ? WindowHit::BottomRight : WindowHit::Bottom;
        }
        else if (left || right)
        {
            hit = left ? WindowHit::Left : WindowHit::Right;
        }
    }

    if (m_customFrame && hit == WindowHit::Client && !m_titleBar.inputBlocked && y >= 0 && y < m_titleBar.height
        && x >= m_titleBar.menuEnd && x < float(extent.width) - m_titleBar.controlsWidth)
    {
        hit = WindowHit::Caption;
    }

    return hit;
}
} // namespace zen::platform
