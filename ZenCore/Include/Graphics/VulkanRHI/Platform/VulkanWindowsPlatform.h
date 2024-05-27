#pragma once
#include <vector>
#include "Common/UniquePtr.h"
#define VK_USE_PLATFORM_WIN32_KHR

namespace zen
{
class VulkanInstanceExtension;
class InstanceExtensionArray;

class VulkanWindowsPlatform
{
public:
    static void AddInstanceExtensions(std::vector<UniquePtr<VulkanInstanceExtension>>& extensions);
};
} // namespace zen