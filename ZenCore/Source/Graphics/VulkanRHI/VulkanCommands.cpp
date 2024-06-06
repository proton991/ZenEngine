#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanCommands.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"

namespace zen
{
VkCommandBuffer VulkanCommandBufferManager::CreateCommandBuffer(VkCommandPool        vkCmdPool,
                                                                VkCommandBufferLevel level) const
{
    VkCommandBufferAllocateInfo allocInfo;
    InitVkStruct(allocInfo, VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
    allocInfo.commandPool        = vkCmdPool;
    allocInfo.commandBufferCount = 1;
    allocInfo.level              = level;

    VkCommandBuffer cmdBuffer = VK_NULL_HANDLE;
    VKCHECK(vkAllocateCommandBuffers(m_device->GetVkHandle(), &allocInfo, &cmdBuffer));

    return cmdBuffer;
}

VkCommandBuffer VulkanCommandBufferManager::RequestCommandBufferPrimary(
    CommandPoolHandle cmdPoolHandle)
{
    VulkanCommandPool* cmdPool = reinterpret_cast<VulkanCommandPool*>(cmdPoolHandle.value);
    if (m_activePrimaryCmdBufferCount[cmdPoolHandle] >= m_primaryCmdBuffers[cmdPoolHandle].size())
    {
        VkCommandBuffer vkCmdBuffer =
            CreateCommandBuffer(cmdPool->vkHandle, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        m_activePrimaryCmdBufferCount[cmdPoolHandle]++;
        m_primaryCmdBuffers[cmdPoolHandle].push_back(vkCmdBuffer);
        return m_primaryCmdBuffers[cmdPoolHandle].back();
    }
    return m_primaryCmdBuffers[cmdPoolHandle][m_activePrimaryCmdBufferCount[cmdPoolHandle]++];
}

VkCommandBuffer VulkanCommandBufferManager::RequestCommandBufferSecondary(
    CommandPoolHandle cmdPoolHandle)
{
    VulkanCommandPool* cmdPool = reinterpret_cast<VulkanCommandPool*>(cmdPoolHandle.value);
    if (m_activeSecondaryCmdBufferCount[cmdPoolHandle] >=
        m_secondaryCmdBuffers[cmdPoolHandle].size())
    {
        VkCommandBuffer vkCmdBuffer =
            CreateCommandBuffer(cmdPool->vkHandle, VK_COMMAND_BUFFER_LEVEL_SECONDARY);
        m_activeSecondaryCmdBufferCount[cmdPoolHandle]++;
        m_secondaryCmdBuffers[cmdPoolHandle].push_back(vkCmdBuffer);
        return m_secondaryCmdBuffers[cmdPoolHandle].back();
    }
    return m_secondaryCmdBuffers[cmdPoolHandle][m_activeSecondaryCmdBufferCount[cmdPoolHandle]++];
}

CommandPoolHandle VulkanRHI::CreateCommandPool(const uint32_t queueFamilyIndex)
{
    VulkanCommandPool* cmdPool = new VulkanCommandPool();

    VkCommandPoolCreateInfo cmdPoolCI;
    InitVkStruct(cmdPoolCI, VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
    cmdPoolCI.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT; // reset whole pool
    cmdPoolCI.queueFamilyIndex = queueFamilyIndex;
    VKCHECK(vkCreateCommandPool(m_device->GetVkHandle(), &cmdPoolCI, nullptr, &cmdPool->vkHandle));

    cmdPool->queueFamilyIndex = queueFamilyIndex;

    return CommandPoolHandle(cmdPool);
}

void VulkanRHI::ResetCommandPool(CommandPoolHandle commandPoolHandle)
{
    VulkanCommandPool* cmdPool = reinterpret_cast<VulkanCommandPool*>(commandPoolHandle.value);
    vkResetCommandPool(m_device->GetVkHandle(), cmdPool->vkHandle, 0);
}

void VulkanRHI::DestroyCommandPool(CommandPoolHandle commandPoolHandle)
{
    VulkanCommandPool* cmdPool = reinterpret_cast<VulkanCommandPool*>(commandPoolHandle.value);
    vkDestroyCommandPool(m_device->GetVkHandle(), cmdPool->vkHandle, nullptr);
}

CommandBufferHandle VulkanRHI::GetOrCreateCommandBuffer(CommandPoolHandle        cmdPoolHandle,
                                                        const CommandBufferLevel level)
{
    VkCommandBuffer vkCmdBuffer = VK_NULL_HANDLE;
    if (level == CommandBufferLevel::PRIMARY)
    {
        vkCmdBuffer = m_cmdBufferManager->RequestCommandBufferPrimary(cmdPoolHandle);
    }
    else if (level == CommandBufferLevel::SECONDARY)
    {
        vkCmdBuffer = m_cmdBufferManager->RequestCommandBufferSecondary(cmdPoolHandle);
    }

    return CommandBufferHandle(vkCmdBuffer);
}

} // namespace zen