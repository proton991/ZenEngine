#pragma once
#include "VulkanHeaders.h"
#include "Templates/VectorView.h"
#include <algorithm>

namespace zen
{
struct VulkanQueueLocation
{
    uint32_t familyIndex{UINT32_MAX};
    uint32_t queueIndex{0};
};

struct VulkanQueueSelection
{
    VulkanQueueLocation graphics;
    VulkanQueueLocation compute;
    VulkanQueueLocation transfer;

    bool IsValid() const
    {
        return graphics.familyIndex != UINT32_MAX && compute.familyIndex != UINT32_MAX &&
            transfer.familyIndex != UINT32_MAX;
    }

    uint32_t GetRequestedQueueCount(uint32_t familyIndex) const
    {
        uint32_t count = 0;
        for (const VulkanQueueLocation& queue : {graphics, compute, transfer})
        {
            if (queue.familyIndex == familyIndex && familyIndex != UINT32_MAX)
            {
                count = std::max(count, queue.queueIndex + 1);
            }
        }
        return count;
    }
};

inline VulkanQueueSelection SelectVulkanQueues(VectorView<const VkQueueFamilyProperties> families)
{
    VulkanQueueSelection selection;
    const VkQueueFlags graphicsFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    for (uint32_t i = 0; i < families.size(); ++i)
    {
        if (families[i].queueCount != 0 &&
            (families[i].queueFlags & graphicsFlags) == graphicsFlags)
        {
            selection.graphics = {i, 0};
            break;
        }
    }

    if (selection.graphics.familyIndex != UINT32_MAX)
    {
        for (uint32_t i = 0; i < families.size(); ++i)
        {
            const VkQueueFlags flags = families[i].queueFlags;
            if (families[i].queueCount != 0)
            {
                if (i != selection.graphics.familyIndex && (flags & VK_QUEUE_COMPUTE_BIT) != 0 &&
                    (selection.compute.familyIndex == UINT32_MAX ||
                     ((families[selection.compute.familyIndex].queueFlags &
                       VK_QUEUE_GRAPHICS_BIT) != 0 &&
                      (flags & VK_QUEUE_GRAPHICS_BIT) == 0)))
                {
                    selection.compute = {i, 0};
                }
                if (selection.transfer.familyIndex == UINT32_MAX &&
                    (flags & VK_QUEUE_TRANSFER_BIT) != 0 && (flags & graphicsFlags) == 0)
                {
                    selection.transfer = {i, 0};
                }
            }
        }

        if (selection.compute.familyIndex == UINT32_MAX)
        {
            selection.compute = selection.graphics;
            if (families[selection.graphics.familyIndex].queueCount > 1)
            {
                selection.compute.queueIndex = 1;
            }
        }
        if (selection.transfer.familyIndex == UINT32_MAX)
        {
            selection.transfer = selection.compute;
        }
    }
    return selection;
}
} // namespace zen
