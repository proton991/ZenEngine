#include "Platform/WindowBackend.h"
#include "Utils/Errors.h"
#if defined(ZEN_WIN32)
#    define GLFW_EXPOSE_NATIVE_WIN32
#    include <GLFW/glfw3native.h>
#    include <commctrl.h>
#    include <dwmapi.h>
#endif

namespace zen::platform
{
class FallbackFrame
{
public:
    explicit FallbackFrame(GLFWwindow* window) : window(window) {}

    ~FallbackFrame()
    {
#if defined(ZEN_WIN32)
        if (handle != nullptr && enabled)
        {
            RemoveWindowSubclass(handle, WindowProcedure, reinterpret_cast<UINT_PTR>(this));

            enabled = false;

            const MARGINS margins{};

            DwmExtendFrameIntoClientArea(handle, &margins);

            glfwSetWindowAttrib(window, GLFW_DECORATED, wasDecorated);
        }
#endif
    }

#if defined(ZEN_WIN32)
    static LRESULT CALLBACK
    WindowProcedure(HWND handle, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR subclass, DWORD_PTR reference)
    {
        FallbackFrame& state = *reinterpret_cast<FallbackFrame*>(reference);

        LRESULT result       = 0;

        bool handled         = false;

        if (message == WM_NCCALCSIZE && wParam != 0)
        {
            // The whole window is drawable. A maximized client stops at the taskbar's work area.
            if (IsZoomed(handle))
            {
                MONITORINFO monitor{sizeof(MONITORINFO)};

                if (GetMonitorInfoW(MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST), &monitor))
                {
                    reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam)->rgrc[0] = monitor.rcWork;
                }
            }

            handled = true;
        }
        else if (message == WM_NCHITTEST)
        {
            // Signed screen coordinates also support monitors left of or above the primary display.
            POINT point{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};

            ScreenToClient(handle, &point);

            result  = state.HitTest(point);

            handled = true;
        }
        else if (message == WM_NCDESTROY)
        {
            RemoveWindowSubclass(handle, WindowProcedure, subclass);

            state.enabled = false;

            state.handle  = nullptr;
        }

        if (!handled)
        {
            result = DefSubclassProc(handle, message, wParam, lParam);
        }

        return result;
    }

    LRESULT HitTest(POINT point) const
    {
        NativeWindow& owner              = *static_cast<NativeWindow*>(glfwGetWindowUserPointer(window));

        static constexpr LRESULT codes[] = {HTCLIENT, HTCAPTION, HTLEFT,     HTRIGHT,      HTTOP,
                                            HTBOTTOM, HTTOPLEFT, HTTOPRIGHT, HTBOTTOMLEFT, HTBOTTOMRIGHT};

        const LRESULT result             = codes[static_cast<size_t>(owner.HitTest(float(point.x), float(point.y)))];

        return result;
    }

    HWND handle{nullptr};

    int wasDecorated{GLFW_TRUE};
#endif

    GLFWwindow* window;

    bool enabled{false};
};


bool WindowBackend::SetFallbackFrame(NativeWindow& window, bool enabled)
{
    window.CheckThreadOwnership();

    bool success = !enabled;

#if defined(ZEN_WIN32)
    if (enabled && !window.m_frameData)
    {
        FallbackFrame* state = new FallbackFrame(Borrow(window));
        state->handle        = glfwGetWin32Window(state->window);

        state->wasDecorated  = glfwGetWindowAttrib(state->window, GLFW_DECORATED);

        state->enabled = SetWindowSubclass(state->handle, FallbackFrame::WindowProcedure, reinterpret_cast<UINT_PTR>(state),
                                           reinterpret_cast<DWORD_PTR>(state))
                      != FALSE;

        if (state->enabled)
        {
            int width  = 0;

            int height = 0;

            glfwGetWindowSize(state->window, &width, &height);

            // Keep GLFW's client-size accounting consistent with our full-client frame.
            glfwSetWindowAttrib(state->window, GLFW_DECORATED, GLFW_FALSE);

            const LONG_PTR style = GetWindowLongPtrW(state->handle, GWL_STYLE);

            SetWindowLongPtrW(state->handle, GWL_STYLE, style | WS_THICKFRAME | WS_MAXIMIZEBOX);

            const MARGINS margins{1, 1, 1, 1};

            DwmExtendFrameIntoClientArea(state->handle, &margins);

            const BOOL dark = TRUE;

            // Ignored on Windows versions that do not support dark non-client borders.
            constexpr DWORD useImmersiveDarkMode = 20;

            DwmSetWindowAttribute(state->handle, useImmersiveDarkMode, &dark, sizeof(dark));

            SetWindowPos(state->handle, nullptr, 0, 0, width, height,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
        else
        {
            LOGW("Custom editor title bar unavailable; keeping native window decoration");
        }
        success = state->enabled;

        if (success)
        {
            window.m_frameData = state;
        }
        else
        {
            delete state;
        }
    }
    else if (!enabled)
    {
        delete static_cast<FallbackFrame*>(window.m_frameData);

        window.m_frameData = nullptr;
    }
    else
    {
        success = true;
    }
#endif
    window.m_customFrame = enabled && success;

    return success;
}
} // namespace zen::platform
