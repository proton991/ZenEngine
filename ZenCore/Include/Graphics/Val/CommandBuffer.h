#pragma once
#include <vulkan/vulkan.h>
#include "DeviceObject.h"

namespace zen::val
{
class CommandPool;
class Image;
class Buffer;
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

    void CopyBuffer(Buffer* srcBuffer, size_t srcOffset, Buffer* dstBuffer, size_t dstOffset, size_t byteSize);

    template <class... Buffers>
    void BindVertexBuffers(Buffers&... vertexBuffers)
    {
        constexpr size_t bufferCount          = sizeof...(Buffers);
        std::array       buffers              = {vertexBuffers.GetHandle()...};
        uint64_t         offsets[bufferCount] = {0};
        vkCmdBindVertexBuffers(m_handle, 0, bufferCount, buffers.data(), offsets);
    }

    void DrawVertices(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex = 0, uint32_t firstInstance = 0);

    void SetViewport(float width, float height);

    void SetScissor(uint32_t width, uint32_t height);

private:
    CommandPool& m_cmdPool;

    const VkCommandBufferLevel m_level;
};
} // namespace zen::val