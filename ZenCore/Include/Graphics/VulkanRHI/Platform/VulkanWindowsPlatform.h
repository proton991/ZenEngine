#pragma once
#if defined(ZEN_WIN32)
#    include "VulkanPlatformCommon.h"
#    include "Templates/HeapVector.h"
#    include "Utils/UniquePtr.h"
#    define VK_USE_PLATFORM_WIN32_KHR

namespace zen
{
class VulkanInstanceExtension;

class VulkanWindowsPlatform
{
public:
    static void AddInstanceExtensions(HeapVector<UniquePtr<VulkanInstanceExtension>>& extensions);

    static VkSurfaceKHR CreateSurface(VkInstance instance, platform::NativeWindow& window);

    static void DestroySurface(VkInstance instance, VkSurfaceKHR surface);
};
typedef VulkanWindowsPlatform VulkanPlatform;
} // namespace zen

#endif