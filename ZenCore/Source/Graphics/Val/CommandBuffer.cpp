#include "Graphics/Val/CommandBuffer.h"
#include "Graphics/Val/CommandPool.h"
#include "Graphics/Val/Device.h"
#include "Common/Errors.h"

namespace zen::val
{
CommandBuffer::CommandBuffer(CommandPool& cmdPool, VkCommandBufferLevel level) :
    m_cmdPool(cmdPool), m_level(level)
{
    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};

    allocInfo.commandPool        = m_cmdPool.GetHandle();
    allocInfo.commandBufferCount = 1;
    allocInfo.level              = level;

    CHECK_VK_ERROR_AND_THROW(vkAllocateCommandBuffers(cmdPool.GetDevice().GetHandle(), &allocInfo, &m_handle), "Failed to allocate command buffer");
}

void CommandBuffer::Reset()
{
    CHECK_VK_ERROR_AND_THROW(vkResetCommandBuffer(m_handle, VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT), "Failed to reset command buffer");
}
} // namespace zen::val