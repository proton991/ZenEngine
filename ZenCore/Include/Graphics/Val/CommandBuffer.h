#pragma once
#include <vulkan/vulkan.h>
#include "DeviceObject.h"

namespace zen::val
{
class CommandPool;
class Image;
class CommandBuffer : public DeviceObject<VkCommandBuffer, VK_OBJECT_TYPE_COMMAND_BUFFER>
{
public:
    CommandBuffer(CommandPool& cmdPool, VkCommandBufferLevel level);

    void Reset();

    void PipelineBarrier(VkPipelineStageFlags srcPipelineStage, VkPipelineStageFlags dstPipelineStage, const std::vector<VkBufferMemoryBarrier>& bufferMemBarriers, const std::vector<VkImageMemoryBarrier>& imageMemBarriers);

    void BlitImage(const Image& srcImage, VkImageUsageFlags srcUsage, const Image& dstImage, VkImageUsageFlags dstUsage);

    void BeginRenderPass(const VkRenderPassBeginInfo& info, VkSubpassContents subpassContents = VK_SUBPASS_CONTENTS_INLINE);

    void EndRenderPass();

    void BindGraphicPipeline(VkPipeline pipeline);

    void BindDescriptorSets(VkPipelineLayout pipelineLayout, const std::vector<VkDescriptorSet>& descriptorSets);

    void Begin();

    void End();

private:
    CommandPool& m_cmdPool;

    const VkCommandBufferLevel m_level;
};
} // namespace zen::val