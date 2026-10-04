#pragma once
#include "VulkanPlatformCommon.h"
#include "Templates/HeapVector.h"

#if defined(ZEN_MACOS)

#    include <vector>
#    include "Utils/UniquePtr.h"
#    define VK_USE_PLATFORM_MACOS_MVK

namespace zen
{
class VulkanRHI;
class VulkanInstanceExtension;
class VulkanMacOSPlatform
{
public:
    static void AddInstanceExtensions(HeapVector<UniquePtr<VulkanInstanceExtension>>& extensions);

    static VkSurfaceKHR CreateSurface(VkInstance instance, platform::NativeWindow& window);

    static void DestroySurface(VkInstance instance, VkSurfaceKHR surface);
};

typedef VulkanMacOSPlatform VulkanPlatform;
} // namespace zen

#endif