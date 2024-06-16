#pragma once
#include "Platform/GlfwWindow.h"

namespace zen::rhi
{
struct VulkanSurface
{
    VkSurfaceKHR surface{VK_NULL_HANDLE};
    uint32_t width{0};
    uint32_t height{0};
};

} // namespace zen::rhi