#pragma once
#include "VulkanHeaders.h"
#include "Common/SmallVector.h"
#include "Graphics/RHI/RHICommands.h"

namespace zen::rhi
{
class VulkanRHI;
class VulkanDevice;
class VulkanCommandBuffer;
class VulkanCommandBufferManager;

class VulkanCommandListContext : public RHICommandListContext
{
public:
    explicit VulkanCommandListContext(VulkanRHI* RHI);

    ~VulkanCommandListContext() override;

    auto* GetCmdBufferManager() const
    {
        return m_cmdBufferMgr;
    }

    auto* GetVkRHI() const
    {
        return m_vkRHI;
    }

private:
    VulkanRHI* m_vkRHI{nullptr};
    VulkanCommandBufferManager* m_cmdBufferMgr{nullptr};
};

class VulkanCommandList : public RHICommandList
{
public:
    explicit VulkanCommandList(VulkanCommandListContext* context);

    ~VulkanCommandList() override;

    void BeginRender() final;
    void EndRender() final;

    void BeginUpload() final;
    void EndUpload() final;

    void AddPipelineBarrier(BitField<PipelineStageBits> srcStages,
                            BitField<PipelineStageBits> dstStages,
                            const std::vector<MemoryTransition>& memoryTransitions,
                            const std::vector<BufferTransition>& bufferTransitions,
                            const std::vector<TextureTransition>& textureTransitions) final;

    void ClearBuffer(BufferHandle bufferHandle, uint32_t offset, uint32_t size) final;

    void CopyBuffer(BufferHandle srcBufferHandle,
                    BufferHandle dstBufferHandle,
                    const BufferCopyRegion& region) final;

    void ClearTexture(TextureHandle textureHandle,
                      const Color& color,
                      const TextureSubResourceRange& range) final;

    void CopyTexture(TextureHandle srcTextureHandle,
                     TextureHandle dstTextureHandle,
                     VectorView<TextureCopyRegion> regions) final;

    void CopyTextureToBuffer(TextureHandle textureHandle,
                             BufferHandle bufferHandle,
                             VectorView<BufferTextureCopyRegion> regions) final;

    void CopyBufferToTexture(BufferHandle bufferHandle,
                             TextureHandle textureHandle,
                             VectorView<BufferTextureCopyRegion> regions) final;

    void ResolveTexture(TextureHandle srcTextureHandle,
                        TextureHandle dstTextureHandle,
                        uint32_t srcLayer,
                        uint32_t srcMipmap,
                        uint32_t dstLayer,
                        uint32_t dstMipmap) final;

    void BindIndexBuffer(BufferHandle bufferHandle, DataFormat format, uint32_t offset) final;

    void BindVertexBuffers(VectorView<BufferHandle> bufferHandles, const uint64_t* offsets) final;

    void BindGfxPipeline(PipelineHandle pipelineHandle) final;

    void BindComputePipeline(PipelineHandle pipelineHandle) final;

    void BeginRenderPass(RenderPassHandle renderPassHandle,
                         FramebufferHandle framebufferHandle,
                         const Rect2<int>& area,
                         VectorView<RenderPassClearValue> clearValues) final;

    void EndRenderPass() final;

    void BeginRenderPassDynamic(const RenderPassLayout& rpLayout,
                                const Rect2<int>& area,
                                VectorView<RenderPassClearValue> clearValues) final;

    void EndRenderPassDynamic() final;

    void Draw(uint32_t vertexCount,
              uint32_t instanceCount,
              uint32_t firstVertex,
              uint32_t firstInstance) final;

    void DrawIndexed(uint32_t indexCount,
                     uint32_t instanceCount,
                     uint32_t firstIndex,
                     uint32_t vertexOffset,
                     uint32_t firstInstance) final;

    void DrawIndexedIndirect(BufferHandle indirectBuffer,
                             uint32_t offset,
                             uint32_t drawCount,
                             uint32_t stride) final;

    void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) final;

    void DispatchIndirect(BufferHandle indirectBuffer, uint32_t offset) final;

    void SetPushConstants(PipelineHandle pipelineHandle, VectorView<uint8_t> data) final;

    void SetViewports(VectorView<Rect2<float>> viewports) final;

    void SetScissors(VectorView<Rect2<int>> scissors) final;

    void SetDepthBias(float depthBiasConstantFactor,
                      float depthBiasClamp,
                      float depthBiasSlopeFactor) final;

    void SetLineWidth(float width) final;

    void SetBlendConstants(const Color& color) final;

    // used by VulkanRHI backends
    VulkanCommandBufferManager* GetCmdBufferManager() const
    {
        return m_cmdBufferManager;
    }

    void ChangeTextureLayout(TextureHandle textureHandle, TextureLayout newLayout) final;

    void GenerateTextureMipmaps(TextureHandle textureHandle) final;

    void ChangeImageLayout(VkImage image,
                           VkImageLayout srcLayout,
                           VkImageLayout dstLayout,
                           const VkImageSubresourceRange& range);

private:
    VulkanRHI* m_vkRHI{nullptr};
    VulkanCommandBufferManager* m_cmdBufferManager{nullptr};
    VulkanCommandBuffer* m_cmdBuffer{nullptr};

    friend class VulkanRHI;
};
} // namespace zen::rhi