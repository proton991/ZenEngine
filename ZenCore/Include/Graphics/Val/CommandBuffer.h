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
    CommandBuffer(CommandPool& cmdPool, VkCommandBufferLevel level, std::string debugName = "");

    void Reset();

    void PipelineBarrier(VkPipelineStageFlags srcPipelineStage, VkPipelineStageFlags dstPipelineStage, const std::vector<VkBufferMemoryBarrier>& bufferMemBarriers, const std::vector<VkImageMemoryBarrier>& imageMemBarriers);

    void BlitImage(const Image& srcImage, VkImageUsageFlags srcUsage, const Image& dstImage, VkImageUsageFlags dstUsage);

private:
    CommandPool&               m_cmdPool;
    const VkCommandBufferLevel m_level;
    VkCommandBuffer            m_handle{VK_NULL_HANDLE};
};
} // namespace zen::val