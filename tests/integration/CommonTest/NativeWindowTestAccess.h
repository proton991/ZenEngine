#pragma once
#include "Platform/WindowBackend.h"
#if defined(ZEN_WIN32)
#    include <Windows.h>
#    if !defined(ZEN_WINDOW_SDL3)
#        define GLFW_EXPOSE_NATIVE_WIN32
#        include <GLFW/glfw3native.h>
#    endif

inline HWND GetTestWindowHandle(zen::platform::NativeWindow& window)
{
#    if defined(ZEN_WINDOW_SDL3)
    return static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(zen::platform::WindowBackend::Borrow(window)),
                                                    SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
#    else
    return glfwGetWin32Window(zen::platform::WindowBackend::Borrow(window));
#    endif
}
#endif
