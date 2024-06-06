#pragma once
#include "VulkanPlatformCommon.h"
#include "Graphics/RHI/RHIDefs.h"

#if defined(ZEN_MACOS)

#    include <vector>
#    include "Common/UniquePtr.h"
#    define VK_USE_PLATFORM_MACOS_MVK

namespace zen
{
class VulkanInstanceExtension;
class InstanceExtensionArray;

struct MacOSWindowData
{
    GLFWwindow* glfwWindow{nullptr};
    uint32_t    width{0};
    uint32_t    height{0};
};


class VulkanMacOSPlatform
{
public:
    static void AddInstanceExtensions(std::vector<UniquePtr<VulkanInstanceExtension>>& extensions);

    static bool VolkInitialize();

    static SurfaceHandle CreateSurface(VkInstance instance, void* windowData);

    static void DestroySurface(VkInstance instance, SurfaceHandle surfaceHandle);
};
} // namespace zen

#endif