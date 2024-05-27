#pragma once
#if defined(ZEN_MACOS)

#    include <vector>
#    include "Common/UniquePtr.h"
#    define VK_USE_PLATFORM_MACOS_MVK

namespace zen
{
class VulkanInstanceExtension;
class InstanceExtensionArray;

class VulkanMacOSPlatform
{
public:
    static void AddInstanceExtensions(std::vector<UniquePtr<VulkanInstanceExtension>>& extensions);
    static bool VolkInitialize();
};
} // namespace zen

#endif