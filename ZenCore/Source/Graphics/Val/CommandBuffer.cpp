#include <utility>

#include "Graphics/Val/CommandBuffer.h"
#include "Graphics/Val/CommandPool.h"
#include "Common/Errors.h"
#include "Common/Helpers.h"

namespace zen::val
{
CommandBuffer::CommandBuffer(CommandPool& cmdPool, VkCommandBufferLevel level, std::string debugName) :
    DeviceObject(cmdPool.GetDevice(), std::move(debugName)), m_cmdPool(cmdPool), m_level(level)
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

void CommandBuffer::PipelineBarrier(VkPipelineStageFlags srcPipelineStage, VkPipelineStageFlags dstPipelineStage, const std::vector<VkBufferMemoryBarrier>& bufferMemBarriers, const std::vector<VkImageMemoryBarrier>& imageMemBarriers)
{
    vkCmdPipelineBarrier(m_handle, srcPipelineStage, dstPipelineStage, VK_DEPENDENCY_BY_REGION_BIT, 0, nullptr, util::ToU32(bufferMemBarriers.size()), bufferMemBarriers.data(), util::ToU32(imageMemBarriers.size()), imageMemBarriers.data());
}
} // namespace zen::val