#include "VulkanWindowBridge.h"
#include "Platform/WindowBackend.h"
#include "Utils/Errors.h"
#if defined(ZEN_WINDOW_SDL3)
#    include <SDL3/SDL_vulkan.h>
#endif

namespace zen
{
void AddWindowInstanceExtensions(HeapVector<UniquePtr<VulkanInstanceExtension>>& extensions)
{
    uint32_t count           = 0;

    const char* const* names = nullptr;

#if defined(ZEN_WINDOW_SDL3)
    // Headless RHI initialization must never initialize a desktop subsystem.
    if (SDL_WasInit(SDL_INIT_VIDEO))
    {
        names = SDL_Vulkan_GetInstanceExtensions(&count);

        VERIFY_EXPR_MSG(names != nullptr, SDL_GetError());
    }
#else
    if (platform::WindowBackend::IsInitialized())
    {
        names = glfwGetRequiredInstanceExtensions(&count);
    }
#endif
    for (uint32_t index = 0; index < count; ++index)
    {
        bool exists = false;

        for (const UniquePtr<VulkanInstanceExtension>& extension : extensions)
        {
            exists |= extension->GetName() == NameID(names[index]);
        }

        if (!exists)
        {
            extensions.emplace_back(MakeUnique<VulkanInstanceExtension>(names[index]));
        }
    }
}

VkSurfaceKHR CreateWindowSurface(VkInstance instance, platform::NativeWindow& window)
{
    window.CheckThreadOwnership();

    VkSurfaceKHR surface = VK_NULL_HANDLE;

#if defined(ZEN_WINDOW_SDL3)
    if (!SDL_Vulkan_CreateSurface(platform::WindowBackend::Borrow(window), instance, nullptr, &surface))
    {
        LOGE("SDL Vulkan surface creation failed: {}", SDL_GetError());
    }
#else
    const VkResult result = glfwCreateWindowSurface(instance, platform::WindowBackend::Borrow(window), nullptr, &surface);

    if (result != VK_SUCCESS)
    {
        LOGE("GLFW fallback surface creation failed: {}", int32_t(result));
    }
#endif
    return surface;
}
} // namespace zen
