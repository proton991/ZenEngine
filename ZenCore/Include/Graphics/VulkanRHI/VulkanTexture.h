#pragma once
#include "VulkanHeaders.h"
#include "VulkanMemory.h"

namespace zen::rhi
{
struct VulkanTexture
{
    static VkImageSubresourceRange GetSubresourceRange(
        VkImageAspectFlags aspect,
        uint32_t baseMipLevel   = 0,
        uint32_t levelCount     = VK_REMAINING_MIP_LEVELS,
        uint32_t baseArrayLayer = 0,
        uint32_t layerCount     = VK_REMAINING_ARRAY_LAYERS)
    {
        VkImageSubresourceRange range{};
        range.aspectMask     = aspect;
        range.baseMipLevel   = baseMipLevel;
        range.levelCount     = levelCount;
        range.baseArrayLayer = baseArrayLayer;
        range.layerCount     = layerCount;
        return range;
    }

    VkImage image{VK_NULL_HANDLE};
    VkImageView imageView{VK_NULL_HANDLE};
    VkImageCreateInfo imageCI{};
    VulkanMemoryAllocation memAlloc{};
    bool isProxy{false};
};
} // namespace zen::rhi