#include "Graphics/Val/CommandBuffer.h"
#include "Graphics/Val/CommandPool.h"
#include "Graphics/Val/Image.h"
#include "Common/Errors.h"
#include "Common/Helpers.h"

namespace zen::val
{
CommandBuffer::CommandBuffer(CommandPool& cmdPool, VkCommandBufferLevel level) :
    DeviceObject(cmdPool.GetDevice()), m_cmdPool(cmdPool), m_level(level)
{
    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};

    allocInfo.commandPool        = m_cmdPool.GetHandle();
    allocInfo.commandBufferCount = 1;
    allocInfo.level              = m_level;

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

void CommandBuffer::BlitImage(const Image& srcImage, VkImageUsageFlags srcUsage, const Image& dstImage, VkImageUsageFlags dstUsage)
{
    std::vector<VkImageMemoryBarrier> barriers(2);

    uint32_t numBarriers = 0;

    VkImageMemoryBarrier barrier1{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier1.srcAccessMask       = Image::UsageToAccessFlags(srcUsage);
    barrier1.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
    barrier1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier1.oldLayout           = Image::UsageToImageLayout(srcUsage);
    barrier1.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier1.image               = srcImage.GetHandle();
    barrier1.subresourceRange    = srcImage.GetSubResourceRange();

    VkImageMemoryBarrier barrier2{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier2.srcAccessMask       = Image::UsageToAccessFlags(dstUsage);
    barrier2.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier2.oldLayout           = Image::UsageToImageLayout(dstUsage);
    barrier2.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier2.image               = dstImage.GetHandle();
    barrier2.subresourceRange    = dstImage.GetSubResourceRange();

    if (srcUsage != VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
        barriers[numBarriers++] = barrier1;
    if (dstUsage != VK_IMAGE_USAGE_TRANSFER_DST_BIT)
        barriers[numBarriers++] = barrier2;
    if (numBarriers > 0)
    {
        VkPipelineStageFlags srcPipelineStage = Image::UsageToPipelineStage(srcUsage) | Image::UsageToPipelineStage(dstUsage);
        VkPipelineStageFlags dstPipelineStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        vkCmdPipelineBarrier(m_handle, srcPipelineStage, dstPipelineStage, 0, 0, nullptr, 0, nullptr, numBarriers, barriers.data());
    }
    VkOffset3D zeroOffset = {0, 0, 0};
    VkOffset3D srcOffset  = {static_cast<int32_t>(srcImage.GetExtent3D().width), static_cast<int32_t>(srcImage.GetExtent3D().height), 1};
    VkOffset3D dstOffset  = {static_cast<int32_t>(dstImage.GetExtent3D().width), static_cast<int32_t>(dstImage.GetExtent3D().height), 1};

    VkImageBlit blitInfo{};
    blitInfo.srcOffsets[0]  = zeroOffset;
    blitInfo.srcOffsets[1]  = srcOffset;
    blitInfo.dstOffsets[0]  = zeroOffset;
    blitInfo.dstOffsets[1]  = dstOffset;
    blitInfo.srcSubresource = srcImage.GetSubresourceLayers();
    blitInfo.dstSubresource = dstImage.GetSubresourceLayers();

    vkCmdBlitImage(m_handle, srcImage.GetHandle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImage.GetHandle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blitInfo, VK_FILTER_LINEAR);
}

void CommandBuffer::BeginRenderPass(const VkRenderPassBeginInfo& info, VkSubpassContents subpassContents)
{
    vkCmdBeginRenderPass(m_handle, &info, subpassContents);
}

void CommandBuffer::EndRenderPass()
{
    vkCmdEndRenderPass(m_handle);
}

void CommandBuffer::BindGraphicPipeline(VkPipeline pipeline)
{
    vkCmdBindPipeline(m_handle, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
}

void CommandBuffer::BindDescriptorSets(VkPipelineLayout pipelineLayout, const std::vector<VkDescriptorSet>& descriptorSets)
{
    vkCmdBindDescriptorSets(m_handle, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, util::ToU32(descriptorSets.size()), descriptorSets.data(), 0, nullptr);
}

void CommandBuffer::Begin()
{
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(m_handle, &beginInfo);
}

void CommandBuffer::End()
{
    vkEndCommandBuffer(m_handle);
}
} // namespace zen::val