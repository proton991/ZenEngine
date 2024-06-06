#pragma once

#include "Common/HashMap.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"

namespace zen
{
struct VulkanCommandPool
{
    VkCommandPool vkHandle{VK_NULL_HANDLE};
    uint32_t      queueFamilyIndex{};
};

class VulkanCommandBufferManager
{
public:
    explicit VulkanCommandBufferManager(VulkanDevice* device) : m_device(device) {}

    VkCommandBuffer RequestCommandBufferPrimary(CommandPoolHandle cmdPoolHandle);

    VkCommandBuffer RequestCommandBufferSecondary(CommandPoolHandle cmdPoolHandle);

private:
    VkCommandBuffer CreateCommandBuffer(VkCommandPool vkCmdPool, VkCommandBufferLevel level) const;

    VulkanDevice* m_device{nullptr};

    HashMap<CommandPoolHandle, std::vector<VkCommandBuffer>> m_primaryCmdBuffers;
    HashMap<CommandPoolHandle, std::vector<VkCommandBuffer>> m_secondaryCmdBuffers;

    HashMap<CommandPoolHandle, uint32_t> m_activePrimaryCmdBufferCount{};
    HashMap<CommandPoolHandle, uint32_t> m_activeSecondaryCmdBufferCount{};
};
} // namespace zen
