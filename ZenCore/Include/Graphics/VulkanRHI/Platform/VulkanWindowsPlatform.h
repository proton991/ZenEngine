#pragma once
#if defined(ZEN_WIN32)
#    include "Graphics/RHI/RHIDefs.h"
#    include "VulkanPlatformCommon.h"
#    include <vector>
#    include "Common/UniquePtr.h"
#    define VK_USE_PLATFORM_WIN32_KHR

namespace zen
{
struct Win32WindowData
{
    GLFWwindow* glfwWindow{nullptr};
    uint32_t width{0};
    uint32_t height{0};
};
typedef Win32WindowData WindowData;
} // namespace zen

namespace zen::rhi
{
class VulkanInstanceExtension;
class InstanceExtensionArray;

class VulkanWindowsPlatform
{
public:
    static void AddInstanceExtensions(std::vector<UniquePtr<VulkanInstanceExtension>>& extensions);

    static VkSurfaceKHR CreateSurface(VkInstance instance, void* windowData);

    static void DestroySurface(VkInstance instance, VkSurfaceKHR surface);
};
typedef VulkanWindowsPlatform VulkanPlatform;
} // namespace zen::rhi

#endif