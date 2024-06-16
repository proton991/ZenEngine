#if defined(ZEN_MACOS)
#    include <dlfcn.h>
#    include "Graphics/VulkanRHI/Platform/VulkanMacOSPlatform.h"
#    include "Graphics/VulkanRHI/VulkanExtension.h"

namespace zen::rhi
{
void VulkanMacOSPlatform::AddInstanceExtensions(
    std::vector<UniquePtr<VulkanInstanceExtension>>& extensions)
{
    extensions.emplace_back(
        MakeUnique<VulkanInstanceExtension>(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME));
    extensions.emplace_back(MakeUnique<VulkanInstanceExtension>("VK_EXT_metal_surface"));
    extensions.emplace_back(
        MakeUnique<VulkanInstanceExtension>(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME));
}

SurfaceHandle VulkanMacOSPlatform::CreateSurface(VkInstance instance, void* data)
{
    MacOSWindowData* windowData  = static_cast<MacOSWindowData*>(data);
    VulkanSurface* vulkanSurface = new VulkanSurface();

    vulkanSurface->width  = windowData->width;
    vulkanSurface->height = windowData->height;
    glfwCreateWindowSurface(instance, windowData->glfwWindow, nullptr, &vulkanSurface->surface);

    return SurfaceHandle(vulkanSurface);
}

void VulkanMacOSPlatform::DestroySurface(VkInstance instance, SurfaceHandle surfaceHandle)
{
    VulkanSurface* vulkanSurface = reinterpret_cast<VulkanSurface*>(surfaceHandle.value);
    if (instance != VK_NULL_HANDLE && vulkanSurface->surface != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(instance, vulkanSurface->surface, nullptr);
    }
}
} // namespace zen::rhi

#endif