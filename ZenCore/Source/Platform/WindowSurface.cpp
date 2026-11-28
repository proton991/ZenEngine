#include "Platform/WindowBackend.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"
#include "ZenRHI/Vulkan/VulkanSurface.h"
#include "Utils/Errors.h"
#if defined(ZEN_WINDOW_SDL3)
#    include <SDL3/SDL_vulkan.h>
#endif

namespace zen::platform
{
namespace
{
std::span<const char* const> SurfaceExtensions()
{
    uint32_t           count{};
    const char* const* names{};
#if defined(ZEN_WINDOW_SDL3)
    if (SDL_WasInit(SDL_INIT_VIDEO))
    {
        names = SDL_Vulkan_GetInstanceExtensions(&count);
        VERIFY_EXPR_MSG(names, SDL_GetError());
    }
#else
    if (WindowBackend::IsInitialized())
    {
        names = glfwGetRequiredInstanceExtensions(&count);
    }
#endif
    return {names, count};
}

VkResult CreateSurface(VkInstance instance, void* context, VkSurfaceKHR* surface)
{
    NativeWindow& window = *static_cast<NativeWindow*>(context);
#if defined(ZEN_WINDOW_SDL3)
    const bool created = SDL_Vulkan_CreateSurface(WindowBackend::Borrow(window), instance, nullptr, surface);
    if (!created)
    {
        LOGE("SDL Vulkan surface creation failed: {}", SDL_GetError());
    }
    return created ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
#else
    return glfwCreateWindowSurface(instance, WindowBackend::Borrow(window), nullptr, surface);
#endif
}

void CheckOwner(void* window)
{
    static_cast<NativeWindow*>(window)->CheckThreadOwnership();
}
} // namespace

RHIWindowHandle NativeWindow::GetRHIWindowHandle()
{
    static constexpr VulkanSurfaceProvider provider{CreateSurface, CheckOwner};
    return {this, &provider};
}

void NativeWindow::RegisterSurfaceExtensions()
{
    // The engine creates its RHI device from RenderConfig; a window means the device must be able to present.
    rc::RenderConfig::GetInstance().rhiDevice.surfaceExtensions = &SurfaceExtensions;
}
} // namespace zen::platform
