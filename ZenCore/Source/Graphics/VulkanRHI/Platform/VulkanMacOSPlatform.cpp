#include "Graphics/VulkanRHI/VulkanRHI.h"
#if defined(ZEN_MACOS)
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

VkSurfaceKHR VulkanMacOSPlatform::CreateSurface(VkInstance instance, void* data)
{
    MacOSWindowData* windowData = static_cast<MacOSWindowData*>(data);
    VkSurfaceKHR surface{VK_NULL_HANDLE};
    glfwCreateWindowSurface(instance, windowData->glfwWindow, nullptr, &surface);

    return surface;
}

void VulkanMacOSPlatform::DestroySurface(VkInstance instance, VkSurfaceKHR surface)
{
    if (instance != VK_NULL_HANDLE && surface != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(instance, surface, nullptr);
    }
}
} // namespace zen::rhi

#endif