#if defined(ZEN_WIN32)

#    include "Graphics/VulkanRHI/Platform/VulkanWindowsPlatform.h"
#    include "Graphics/VulkanRHI/VulkanExtension.h"
#    include "Graphics/VulkanRHI/VulkanHeaders.h"

namespace zen
{
void VulkanWindowsPlatform::AddInstanceExtensions(
    std::vector<UniquePtr<VulkanInstanceExtension>>& extensions)
{
    extensions.emplace_back(MakeUnique<VulkanInstanceExtension>("VK_KHR_win32_surface"));
    extensions.emplace_back(
        MakeUnique<VulkanInstanceExtension>(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME));
}

SurfaceHandle VulkanWindowsPlatform::CreateSurface(VkInstance instance, void* data)
{
    Win32WindowData* windowData    = static_cast<Win32WindowData*>(data);
    VulkanSurface*   vulkanSurface = new VulkanSurface();

    vulkanSurface->width  = windowData->width;
    vulkanSurface->height = windowData->height;
    glfwCreateWindowSurface(instance, windowData->glfwWindow, nullptr, &vulkanSurface->surface);

    return SurfaceHandle(vulkanSurface);
}
void VulkanWindowsPlatform::DestroySurface(VkInstance instance, SurfaceHandle surfaceHandle)
{
    VulkanSurface* vulkanSurface = reinterpret_cast<VulkanSurface*>(surfaceHandle.value);
    if (instance != VK_NULL_HANDLE && vulkanSurface->surface != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(instance, vulkanSurface->surface, nullptr);
    }
}
} // namespace zen

#endif