#pragma once
#if defined(ZEN_WIN32)
#    include "Graphics/RHI/RHIDefs.h"
#    include "VulkanPlatformCommon.h"
#    include <vector>
#    include "Common/UniquePtr.h"
#    define VK_USE_PLATFORM_WIN32_KHR

namespace zen
{
class VulkanInstanceExtension;
class InstanceExtensionArray;
struct Win32WindowData
{
    GLFWwindow* glfwWindow{nullptr};
    uint32_t    width{0};
    uint32_t    height{0};
};
class VulkanWindowsPlatform
{
public:
    static void AddInstanceExtensions(std::vector<UniquePtr<VulkanInstanceExtension>>& extensions);

    static SurfaceHandle CreateSurface(VkInstance instance, void* data);

    static void DestroySurface(VkInstance instance, SurfaceHandle surfaceHandle);
};
typedef VulkanWindowsPlatform VulkanPlatform;
typedef Win32WindowData       WindowData;
} // namespace zen

#endif