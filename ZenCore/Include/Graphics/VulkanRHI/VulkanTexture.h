#pragma once
#include "VulkanHeaders.h"
#include "VulkanMemory.h"

namespace zen::rhi
{
struct VulkanTexture
{
    VkImage image{VK_NULL_HANDLE};
    VkImageView imageView{VK_NULL_HANDLE};
    VkImageCreateInfo imageCI{};
    VulkanMemoryAllocation memAlloc{};
};
} // namespace zen::rhi