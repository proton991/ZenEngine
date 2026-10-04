#pragma once
#include "Graphics/VulkanRHI/VulkanExtension.h"
#include "Platform/NativeWindow.h"
namespace zen
{
void AddWindowInstanceExtensions(HeapVector<UniquePtr<VulkanInstanceExtension>>& extensions);

VkSurfaceKHR CreateWindowSurface(VkInstance instance, platform::NativeWindow& window);
} // namespace zen
