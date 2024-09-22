#if defined(ZEN_WIN32)

#    include "Graphics/VulkanRHI/Platform/VulkanWindowsPlatform.h"
#    include "Graphics/VulkanRHI/VulkanExtension.h"
#    include "Graphics/VulkanRHI/VulkanHeaders.h"

namespace zen::rhi
{
void VulkanWindowsPlatform::AddInstanceExtensions(
    std::vector<UniquePtr<VulkanInstanceExtension>>& extensions)
{
    extensions.emplace_back(MakeUnique<VulkanInstanceExtension>("VK_KHR_win32_surface"));
    extensions.emplace_back(
        MakeUnique<VulkanInstanceExtension>(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME));
}

VkSurfaceKHR VulkanWindowsPlatform::CreateSurface(VkInstance instance, void* data)
{
    Win32WindowData* windowData = static_cast<Win32WindowData*>(data);
    VkSurfaceKHR surface{VK_NULL_HANDLE};
    glfwCreateWindowSurface(instance, windowData->glfwWindow, nullptr, &surface);

    return surface;
}

void VulkanWindowsPlatform::DestroySurface(VkInstance instance, VkSurfaceKHR surface)
{
    if (instance != VK_NULL_HANDLE && surface != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(instance, surface, nullptr);
    }
}
} // namespace zen::rhi

#endif