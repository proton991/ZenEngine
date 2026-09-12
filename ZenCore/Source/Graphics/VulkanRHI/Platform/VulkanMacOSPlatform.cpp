#include "Utils/Errors.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#if defined(ZEN_MACOS)
#    include "Graphics/VulkanRHI/Platform/VulkanMacOSPlatform.h"
#    include "Graphics/VulkanRHI/VulkanExtension.h"

namespace zen
{
void VulkanMacOSPlatform::AddInstanceExtensions(
    HeapVector<UniquePtr<VulkanInstanceExtension>>& extensions)
{
#    if defined(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)
    extensions.emplace_back(
        MakeUnique<VulkanInstanceExtension>(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME));
#    endif
    extensions.emplace_back(MakeUnique<VulkanInstanceExtension>("VK_EXT_metal_surface"));
    extensions.emplace_back(
        MakeUnique<VulkanInstanceExtension>(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME));
}

VkSurfaceKHR VulkanMacOSPlatform::CreateSurface(VkInstance instance, void* pData)
{
    MacOSWindowData* pWindowData = static_cast<MacOSWindowData*>(pData);
    VkSurfaceKHR surface{VK_NULL_HANDLE};
    const VkResult result =
        glfwCreateWindowSurface(instance, pWindowData->pGlfwWindow, nullptr, &surface);
    if (result != VK_SUCCESS)
    {
        LOG_ERROR_AND_THROW("glfwCreateWindowSurface failed: {}", int32_t(result));
    }

    return surface;
}

void VulkanMacOSPlatform::DestroySurface(VkInstance instance, VkSurfaceKHR surface)
{
    if (instance != VK_NULL_HANDLE && surface != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(instance, surface, nullptr);
    }
}
} // namespace zen

#endif