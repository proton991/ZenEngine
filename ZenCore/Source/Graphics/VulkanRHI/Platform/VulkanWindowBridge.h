#pragma once
#include "Graphics/VulkanRHI/VulkanExtension.h"
#include "Platform/NativeWindow.h"
#include "Graphics/RHI/RHIError.h"
namespace zen
{
void AddWindowInstanceExtensions(HeapVector<UniquePtr<VulkanInstanceExtension>>& extensions);

RHIResult<VkSurfaceKHR> CreateWindowSurface(VkInstance instance, platform::NativeWindow& window);
} // namespace zen
