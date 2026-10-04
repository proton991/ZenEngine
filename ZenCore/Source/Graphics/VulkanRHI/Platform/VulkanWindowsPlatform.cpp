#include "Utils/Errors.h"
#include "VulkanWindowBridge.h"
#if defined(ZEN_WIN32)

#    include "Graphics/VulkanRHI/Platform/VulkanWindowsPlatform.h"
#    include "Graphics/VulkanRHI/VulkanExtension.h"
#    include "Graphics/VulkanRHI/VulkanHeaders.h"

namespace zen
{
void VulkanWindowsPlatform::AddInstanceExtensions(HeapVector<UniquePtr<VulkanInstanceExtension>>& extensions)
{
    extensions.emplace_back(MakeUnique<VulkanInstanceExtension>("VK_KHR_win32_surface"));

    AddWindowInstanceExtensions(extensions);
}

VkSurfaceKHR VulkanWindowsPlatform::CreateSurface(VkInstance instance, platform::NativeWindow& window)
{
    return CreateWindowSurface(instance, window);
}

void VulkanWindowsPlatform::DestroySurface(VkInstance instance, VkSurfaceKHR surface)
{
    if (instance != VK_NULL_HANDLE && surface != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(instance, surface, nullptr);
    }
}
} // namespace zen

#endif