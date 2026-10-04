#pragma once
#include "Graphics/VulkanRHI/VulkanHeaders.h"
#include "Platform/NativeWindow.h"

namespace zen
{
struct VulkanSurface
{
    VkSurfaceKHR surface{VK_NULL_HANDLE};
    uint32_t     width{0};
    uint32_t     height{0};
};

} // namespace zen
