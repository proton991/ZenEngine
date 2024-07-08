#pragma once
#include <vulkan/vulkan.h>
#include "DeviceObject.h"

namespace zen::val
{
class CommandPool;
class Image;
enum class ImageUsage : uint32_t;
class Buffer;
class CommandBuffer : public DeviceObject<VkCommandBuffer, VK_OBJECT_TYPE_COMMAND_BUFFER>
{
public:
    struct InheritanceInfo
    {
        VkRenderPass renderPassHandle{VK_NULL_HANDLE};
        VkFramebuffer framebufferHandle{VK_NULL_HANDLE};
    };

    CommandBuffer(CommandPool& cmdPool, VkCommandBufferLevel level);

    void Reset();

    void PipelineBarrier(VkPipelineStageFlags srcPipelineStage,
                         VkPipelineStageFlags dstPipelineStage,
                         const std::vector<VkBufferMemoryBarrier>& bufferMemBarriers,
                         const std::vector<VkImageMemoryBarrier>& imageMemBarriers);

    void BlitImage(const Image& srcImage,
                   ImageUsage srcUsage,
                   const Image& dstImage,
                   ImageUsage dstUsage);

    void BeginRenderPass(const VkRenderPassBeginInfo& info,
                         VkSubpassContents subpassContents = VK_SUBPASS_CONTENTS_INLINE);

    void EndRenderPass();

    void BindGraphicPipeline(VkPipeline pipeline);

    void BindDescriptorSets(VkPipelineLayout pipelineLayout,
                            const std::vector<VkDescriptorSet>& descriptorSets);

    /**
     * Begin a primary command buffer
     * @param flags default value VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
     */
    void Begin(VkCommandBufferUsageFlags flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

    /**
      *
      * @param inheritanceInfo primary buffer's inheritance info
      * @param flags default value VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
      * @param subpassIndex current subpass index
      */
    void Begin(const InheritanceInfo& inheritanceInfo,
               VkCommandBufferUsageFlags flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
               // default 0
               uint32_t subpassIndex = 0);

    void End();

    void CopyBuffer(Buffer* srcBuffer,
                    size_t srcOffset,
                    Buffer* dstBuffer,
                    size_t dstOffset,
                    size_t byteSize);

    void CopyBufferToImage(Buffer* srcBuffer,
                           size_t srcOffset,
                           Image* dstImage,
                           uint32_t mipLevel = 0,
                           uint32_t layer    = 0);

    void TransferLayout(Image* image, ImageUsage srcUsage, ImageUsage dstUsage);

    template <class... Buffers> void BindVertexBuffers(Buffers&... vertexBuffers)
    {
        constexpr size_t bufferCount  = sizeof...(Buffers);
        std::array buffers            = {vertexBuffers.GetHandle()...};
        uint64_t offsets[bufferCount] = {0};
        vkCmdBindVertexBuffers(m_handle, 0, bufferCount, buffers.data(), offsets);
    }

    void BindIndexBuffer(const Buffer& buffer, VkIndexType indexType);

    void DrawVertices(uint32_t vertexCount,
                      uint32_t instanceCount,
                      uint32_t firstVertex   = 0,
                      uint32_t firstInstance = 0);

    void DrawIndices(uint32_t indexCount,
                     uint32_t instanceCount,
                     uint32_t firstIndex    = 0,
                     int32_t vertexOffset   = 0,
                     uint32_t firstInstance = 0);

    void SetViewport(float width, float height);

    void SetScissor(uint32_t width, uint32_t height);

    static VkImageMemoryBarrier GetImageBarrier(ImageUsage srcUsage,
                                                ImageUsage dstUsage,
                                                const val::Image* image);

    template <class T> void PushConstants(VkPipelineLayout pipelineLayout,
                                          VkShaderStageFlags shaderStage,
                                          const T* constants)
    {
        PushConstants(pipelineLayout, shaderStage, reinterpret_cast<const uint8_t*>(constants),
                      sizeof(T));
    }

    void ExecuteCommands(std::vector<val::CommandBuffer*>& secondaryCmdBuffers);

    void ExecuteCommand(val::CommandBuffer* secondaryCmdBuffers);

    const auto& GetInheritanceInfo() const
    {
        return m_inheritanceInfo;
    }

private:
    void PushConstants(VkPipelineLayout pipelineLayout,
                       VkShaderStageFlags shaderStage,
                       const uint8_t* data,
                       size_t size);

    CommandPool& m_cmdPool;

    const VkCommandBufferLevel m_level;

    InheritanceInfo m_inheritanceInfo;

    const size_t m_maxPushConstantsSize;
};
} // namespace zen::val