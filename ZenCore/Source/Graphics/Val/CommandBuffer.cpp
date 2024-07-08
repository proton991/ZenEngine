#include "Graphics/Val/CommandBuffer.h"
#include "Graphics/Val/CommandPool.h"
#include "Graphics/Val/Image.h"
#include "Graphics/Val/Buffer.h"
#include "Common/Errors.h"
#include "Common/Helpers.h"

namespace zen::val
{
CommandBuffer::CommandBuffer(CommandPool& cmdPool, VkCommandBufferLevel level) :
    DeviceObject(cmdPool.GetDevice()),
    m_cmdPool(cmdPool),
    m_level(level),
    m_maxPushConstantsSize(m_device.GetGPUProperties().limits.maxPushConstantsSize)
{
    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};

    allocInfo.commandPool        = m_cmdPool.GetHandle();
    allocInfo.commandBufferCount = 1;
    allocInfo.level              = m_level;

    CHECK_VK_ERROR_AND_THROW(
        vkAllocateCommandBuffers(cmdPool.GetDevice().GetHandle(), &allocInfo, &m_handle),
        "Failed to allocate command buffer");
}

void CommandBuffer::Reset()
{
    CHECK_VK_ERROR_AND_THROW(
        vkResetCommandBuffer(m_handle, VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT),
        "Failed to reset command buffer");
}

void CommandBuffer::PipelineBarrier(VkPipelineStageFlags srcPipelineStage,
                                    VkPipelineStageFlags dstPipelineStage,
                                    const std::vector<VkBufferMemoryBarrier>& bufferMemBarriers,
                                    const std::vector<VkImageMemoryBarrier>& imageMemBarriers)
{
    vkCmdPipelineBarrier(m_handle, srcPipelineStage, dstPipelineStage, VK_DEPENDENCY_BY_REGION_BIT,
                         0, nullptr, util::ToU32(bufferMemBarriers.size()),
                         bufferMemBarriers.data(), util::ToU32(imageMemBarriers.size()),
                         imageMemBarriers.data());
}

void CommandBuffer::BlitImage(const Image& srcImage,
                              ImageUsage srcUsage,
                              const Image& dstImage,
                              ImageUsage dstUsage)
{
    std::vector<VkImageMemoryBarrier> barriers(2);

    uint32_t numBarriers = 0;

    VkImageMemoryBarrier toTransferSrc =
        GetImageBarrier(srcUsage, ImageUsage::TransferSrc, &srcImage);
    VkImageMemoryBarrier toTransferDst =
        GetImageBarrier(dstUsage, ImageUsage::TransferDst, &dstImage);

    if (srcUsage != val::ImageUsage::TransferSrc)
        barriers[numBarriers++] = toTransferSrc;
    if (dstUsage != val::ImageUsage::TransferDst)
        barriers[numBarriers++] = toTransferDst;
    if (numBarriers > 0)
    {
        VkPipelineStageFlags srcPipelineStage =
            Image::UsageToPipelineStage(srcUsage) | Image::UsageToPipelineStage(dstUsage);
        VkPipelineStageFlags dstPipelineStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        vkCmdPipelineBarrier(m_handle, srcPipelineStage, dstPipelineStage, 0, 0, nullptr, 0,
                             nullptr, numBarriers, barriers.data());
    }
    VkOffset3D zeroOffset = {0, 0, 0};
    VkOffset3D srcOffset  = {static_cast<int32_t>(srcImage.GetExtent3D().width),
                             static_cast<int32_t>(srcImage.GetExtent3D().height), 1};
    VkOffset3D dstOffset  = {static_cast<int32_t>(dstImage.GetExtent3D().width),
                             static_cast<int32_t>(dstImage.GetExtent3D().height), 1};

    VkImageBlit blitInfo{};
    blitInfo.srcOffsets[0]  = zeroOffset;
    blitInfo.srcOffsets[1]  = srcOffset;
    blitInfo.dstOffsets[0]  = zeroOffset;
    blitInfo.dstOffsets[1]  = dstOffset;
    blitInfo.srcSubresource = srcImage.GetSubresourceLayers();
    blitInfo.dstSubresource = dstImage.GetSubresourceLayers();

    vkCmdBlitImage(m_handle, srcImage.GetHandle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dstImage.GetHandle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blitInfo,
                   VK_FILTER_LINEAR);
}

void CommandBuffer::BeginRenderPass(const VkRenderPassBeginInfo& info,
                                    VkSubpassContents subpassContents)
{
    vkCmdBeginRenderPass(m_handle, &info, subpassContents);
    m_inheritanceInfo.framebufferHandle = info.framebuffer;
    m_inheritanceInfo.renderPassHandle  = info.renderPass;
}

void CommandBuffer::EndRenderPass()
{
    vkCmdEndRenderPass(m_handle);
}

void CommandBuffer::BindGraphicPipeline(VkPipeline pipeline)
{
    vkCmdBindPipeline(m_handle, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
}

void CommandBuffer::BindDescriptorSets(VkPipelineLayout pipelineLayout,
                                       const std::vector<VkDescriptorSet>& descriptorSets)
{
    vkCmdBindDescriptorSets(m_handle, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0,
                            util::ToU32(descriptorSets.size()), descriptorSets.data(), 0, nullptr);
}

void CommandBuffer::Begin(VkCommandBufferUsageFlags flags)
{
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = flags;
    CHECK_VK_ERROR(vkBeginCommandBuffer(m_handle, &beginInfo), "Failed to begin command buffer!")
}

void CommandBuffer::Begin(const InheritanceInfo& inheritanceInfo,
                          VkCommandBufferUsageFlags flags,
                          uint32_t subpassIndex)
{
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    // for secondary command buffers
    VkCommandBufferInheritanceInfo inheritance = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
    beginInfo.flags = flags;

    if (m_level == VK_COMMAND_BUFFER_LEVEL_SECONDARY)
    {
        inheritance.renderPass  = inheritanceInfo.renderPassHandle;
        inheritance.framebuffer = inheritanceInfo.framebufferHandle;
        inheritance.subpass     = subpassIndex;
    }
    beginInfo.pInheritanceInfo = &inheritance;
    CHECK_VK_ERROR(vkBeginCommandBuffer(m_handle, &beginInfo), "Failed to begin command buffer!")
}

void CommandBuffer::End()
{
    vkEndCommandBuffer(m_handle);
}

void CommandBuffer::CopyBuffer(Buffer* srcBuffer,
                               size_t srcOffset,
                               Buffer* dstBuffer,
                               size_t dstOffset,
                               size_t byteSize)
{
    ASSERT(srcOffset + byteSize <= srcBuffer->GetSize());
    ASSERT(dstOffset + byteSize <= dstBuffer->GetSize());
    VkBufferCopy bufferCopy{};
    bufferCopy.srcOffset = srcOffset;
    bufferCopy.dstOffset = dstOffset;
    bufferCopy.size      = byteSize;
    vkCmdCopyBuffer(m_handle, srcBuffer->GetHandle(), dstBuffer->GetHandle(), 1, &bufferCopy);
}

void CommandBuffer::CopyBufferToImage(Buffer* srcBuffer,
                                      size_t srcOffset,
                                      Image* dstImage,
                                      uint32_t mipLevel,
                                      uint32_t layer)
{
    VkImageMemoryBarrier toTransferDstBarrier =
        GetImageBarrier(ImageUsage::Undefined, ImageUsage::TransferDst, dstImage);
    PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, {},
                    {toTransferDstBarrier});

    auto imageSubresourceLayers           = dstImage->GetSubresourceLayers();
    imageSubresourceLayers.mipLevel       = mipLevel;
    imageSubresourceLayers.baseArrayLayer = layer;

    VkBufferImageCopy bufferImageCopy{};
    bufferImageCopy.bufferOffset      = srcOffset;
    bufferImageCopy.bufferImageHeight = 0;
    bufferImageCopy.bufferRowLength   = 0;
    bufferImageCopy.imageSubresource  = imageSubresourceLayers;
    bufferImageCopy.imageOffset       = VkOffset3D{0, 0, 0};
    bufferImageCopy.imageExtent       = dstImage->GetExtent3D();

    vkCmdCopyBufferToImage(m_handle, srcBuffer->GetHandle(), dstImage->GetHandle(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bufferImageCopy);
}

void CommandBuffer::TransferLayout(Image* image, ImageUsage srcUsage, ImageUsage dstUsage)
{
    VkImageMemoryBarrier barrier = GetImageBarrier(srcUsage, dstUsage, image);
    PipelineBarrier(Image::UsageToPipelineStage(srcUsage), Image::UsageToPipelineStage(dstUsage),
                    {}, {barrier});
}

void CommandBuffer::BindIndexBuffer(const Buffer& buffer, VkIndexType indexType)
{
    vkCmdBindIndexBuffer(m_handle, buffer.GetHandle(), 0, indexType);
}

void CommandBuffer::DrawVertices(uint32_t vertexCount,
                                 uint32_t instanceCount,
                                 uint32_t firstVertex,
                                 uint32_t firstInstance)
{
    vkCmdDraw(m_handle, vertexCount, instanceCount, firstVertex, firstInstance);
}

void CommandBuffer::DrawIndices(uint32_t indexCount,
                                uint32_t instanceCount,
                                uint32_t firstIndex,
                                int32_t vertexOffset,
                                uint32_t firstInstance)
{
    vkCmdDrawIndexed(m_handle, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void CommandBuffer::SetViewport(float width, float height)
{
    VkViewport vp{};
    vp.width    = width;
    vp.height   = height;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(m_handle, 0, 1, &vp);
}

void CommandBuffer::SetScissor(uint32_t width, uint32_t height)
{
    VkRect2D scissor{};
    scissor.extent.width  = width;
    scissor.extent.height = height;
    vkCmdSetScissor(m_handle, 0, 1, &scissor);
}

VkImageMemoryBarrier CommandBuffer::GetImageBarrier(ImageUsage srcUsage,
                                                    ImageUsage dstUsage,
                                                    const val::Image* image)
{
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask       = Image::UsageToAccessFlags(srcUsage);
    barrier.dstAccessMask       = Image::UsageToAccessFlags(dstUsage);
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.oldLayout           = Image::UsageToImageLayout(srcUsage);
    barrier.newLayout           = Image::UsageToImageLayout(dstUsage);
    barrier.image               = image->GetHandle();
    barrier.subresourceRange    = image->GetSubResourceRange();
    return barrier;
}

void CommandBuffer::PushConstants(VkPipelineLayout pipelineLayout,
                                  VkShaderStageFlags shaderStage,
                                  const uint8_t* data,
                                  size_t size)
{
    std::vector<uint8_t> pushConstants(size);

    std::memcpy(pushConstants.data(), data, size);

    vkCmdPushConstants(m_handle, pipelineLayout, shaderStage, 0, pushConstants.size(),
                       pushConstants.data());
}

void CommandBuffer::ExecuteCommands(std::vector<val::CommandBuffer*>& secondaryCmdBuffers)
{
    std::vector<VkCommandBuffer> secondCmdBufferHandles(secondaryCmdBuffers.size(), VK_NULL_HANDLE);
    std::transform(secondaryCmdBuffers.begin(), secondaryCmdBuffers.end(),
                   secondCmdBufferHandles.begin(),
                   [](const CommandBuffer* cmdBuffer) { return cmdBuffer->GetHandle(); });
    vkCmdExecuteCommands(GetHandle(), util::ToU32(secondaryCmdBuffers.size()),
                         secondCmdBufferHandles.data());
}

void CommandBuffer::ExecuteCommand(val::CommandBuffer* secondaryCmdBuffers)
{
    VkCommandBuffer secondCmdBufferHandle = secondaryCmdBuffers->GetHandle();
    vkCmdExecuteCommands(GetHandle(), 1, &secondCmdBufferHandle);
}
} // namespace zen::val