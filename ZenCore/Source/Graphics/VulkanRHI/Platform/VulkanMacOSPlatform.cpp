#include "Utils/Errors.h"
#include "VulkanWindowBridge.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#if defined(ZEN_MACOS)
#    include "Graphics/VulkanRHI/Platform/VulkanMacOSPlatform.h"
#    include "Graphics/VulkanRHI/VulkanExtension.h"

namespace zen
{
void VulkanMacOSPlatform::AddInstanceExtensions(HeapVector<UniquePtr<VulkanInstanceExtension>>& extensions)
{
#    if defined(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)
    extensions.emplace_back(MakeUnique<VulkanInstanceExtension>(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME));
#    endif
    extensions.emplace_back(MakeUnique<VulkanInstanceExtension>("VK_EXT_metal_surface"));

    AddWindowInstanceExtensions(extensions);
}

VkSurfaceKHR VulkanMacOSPlatform::CreateSurface(VkInstance instance, platform::NativeWindow& window)
{
    return CreateWindowSurface(instance, window);
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