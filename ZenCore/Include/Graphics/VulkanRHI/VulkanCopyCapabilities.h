#pragma once

#include "Graphics/RHI/RHICommon.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"

namespace zen
{
inline RHITextureCopyCapabilities MakeTextureCopyCapabilities(const VkFormatProperties& properties)
{
    const VkFormatFeatureFlags flags = properties.optimalTilingFeatures;

    return {bool(flags & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT),
            bool(flags & VK_FORMAT_FEATURE_TRANSFER_DST_BIT),
            bool(flags & VK_FORMAT_FEATURE_BLIT_SRC_BIT),
            bool(flags & VK_FORMAT_FEATURE_BLIT_DST_BIT),
            bool(flags & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)};
}

inline RHIQueueCopyCapabilities MakeQueueCopyCapabilities(const VkQueueFamilyProperties& properties)
{
    const VkQueueFlags flags = properties.queueFlags;

    // Graphics and compute queues support transfer commands even without the transfer bit.
    return {bool(flags & VK_QUEUE_GRAPHICS_BIT),
            bool(flags & VK_QUEUE_COMPUTE_BIT),
            bool(flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT)),
            {properties.minImageTransferGranularity.width,
             properties.minImageTransferGranularity.height,
             properties.minImageTransferGranularity.depth}};
}
} // namespace zen
