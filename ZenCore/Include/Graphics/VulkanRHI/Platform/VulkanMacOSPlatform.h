#pragma once
#include "VulkanPlatformCommon.h"
#include "Graphics/RHI/RHIDefs.h"

#if defined(ZEN_MACOS)

#    include <vector>
#    include "Common/UniquePtr.h"
#    define VK_USE_PLATFORM_MACOS_MVK

namespace zen
{
struct MacOSWindowData
{
    GLFWwindow* glfwWindow{nullptr};
    uint32_t width{0};
    uint32_t height{0};
};
typedef MacOSWindowData WindowData;
} // namespace zen

namespace zen::rhi
{
class VulkanRHI;
class VulkanInstanceExtension;
class InstanceExtensionArray;
class VulkanMacOSPlatform
{
public:
    static void AddInstanceExtensions(std::vector<UniquePtr<VulkanInstanceExtension>>& extensions);

    static VkSurfaceKHR CreateSurface(VkInstance instance, void* windowData);

    static void DestroySurface(VkInstance instance, VkSurfaceKHR surface);
};

typedef VulkanMacOSPlatform VulkanPlatform;
} // namespace zen::rhi

#endif