#pragma once
// Private integration boundary. Include directories are private to backend adapters.
#include "Platform/NativeWindow.h"
#if defined(ZEN_WINDOW_SDL3)
#    include <SDL3/SDL.h>
#    include <SDL3/SDL_main.h>
#else
#    define GLFW_INCLUDE_NONE
#    include <GLFW/glfw3.h>
#endif

namespace zen::platform
{
class WindowBackend
{
public:
#if defined(ZEN_WINDOW_SDL3)
    static SDL_Window* Borrow(const NativeWindow& window);

    static void Dispatch(const SDL_Event& event);

    static void DispatchToWindow(NativeWindow& window, const SDL_Event& event);
#else
    static GLFWwindow* Borrow(const NativeWindow& window);

    static void InstallCallbacks(NativeWindow& window);
#endif
    static bool IsInitialized();

    static void Acquire();

    static void Register(NativeWindow& window);

    static void Unregister(NativeWindow& window);

    static void DispatchResizes();

    static void Observe(NativeWindow& window, void (*callback)(void*, const void*), void* context);

    static void Deliver(NativeWindow& window, const InputEvent& event);

    static void Resize(NativeWindow& window);

    static void Close(NativeWindow& window);

    static bool SetFallbackFrame(NativeWindow& window, bool enabled);
};
} // namespace zen::platform
