#pragma once
#include "VulkanHeaders.h"
#include "Templates/SmallVector.h"
#include "Graphics/RHI/RHICommands.h"

namespace zen
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

    void AddPipelineBarrier(BitField<RHIPipelineStageBits> srcStages,
                            BitField<RHIPipelineStageBits> dstStages,
                            const std::vector<RHIMemoryTransition>& memoryTransitions,
                            const std::vector<RHIBufferTransition>& bufferTransitions,
                            const std::vector<RHITextureTransition>& textureTransitions) final;

    void ClearBuffer(RHIBuffer* buffer, uint32_t offset, uint32_t size) final;

    void CopyBuffer(RHIBuffer* srcBuffer,
                    RHIBuffer* dstBuffer,
                    const RHIBufferCopyRegion& region) final;

    void ClearTexture(RHITexture* texture,
                      const Color& color,
                      const RHITextureSubResourceRange& range) final;

    void CopyTexture(RHITexture* srcTexture,
                     RHITexture* dstTexture,
                     VectorView<RHITextureCopyRegion> regions) final;

    void CopyTextureToBuffer(RHITexture* texture,
                             RHIBuffer* buffer,
                             VectorView<RHIBufferTextureCopyRegion> regions) final;

    void CopyBufferToTexture(RHIBuffer* buffer,
                             RHITexture* texture,
                             VectorView<RHIBufferTextureCopyRegion> regions) final;

    void ResolveTexture(RHITexture* srcTexture,
                        RHITexture* dstTexture,
                        uint32_t srcLayer,
                        uint32_t srcMipmap,
                        uint32_t dstLayer,
                        uint32_t dstMipmap) final;

    void BindIndexBuffer(RHIBuffer* buffer, DataFormat format, uint32_t offset) final;

    void BindVertexBuffers(VectorView<RHIBuffer*> buffers, const uint64_t* offsets) final;

    // void BindGfxPipeline(PipelineHandle pipelineHandle) final;

    void BindGfxPipeline(RHIPipeline* pipelineHandle,
                         uint32_t numDescriptorSets,
                         RHIDescriptorSet* const* pDescriptorSets) final;

    // void BindComputePipeline(PipelineHandle pipelineHandle) final;

    void BindComputePipeline(RHIPipeline* pipelineHandle,
                             uint32_t numDescriptorSets,
                             const RHIDescriptorSet* const* pDescriptorSets) final;

    void BeginRenderPass(RenderPassHandle renderPassHandle,
                         FramebufferHandle framebuffer,
                         const Rect2<int>& area,
                         VectorView<RHIRenderPassClearValue> clearValues) final;

    void EndRenderPass() final;

    void BeginRenderPassDynamic(const RHIRenderPassLayout& rpLayout,
                                const Rect2<int>& area,
                                VectorView<RHIRenderPassClearValue> clearValues) final;

    void EndRenderPassDynamic() final;

    void BeginRendering(const RHIRenderingLayout* pRenderingLayout) final;

    void EndRendering() final;

    void Draw(uint32_t vertexCount,
              uint32_t instanceCount,
              uint32_t firstVertex,
              uint32_t firstInstance) final;

    void DrawIndexed(uint32_t indexCount,
                     uint32_t instanceCount,
                     uint32_t firstIndex,
                     int32_t vertexOffset,
                     uint32_t firstInstance) final;

    void DrawIndexedIndirect(RHIBuffer* indirectBuffer,
                             uint32_t offset,
                             uint32_t drawCount,
                             uint32_t stride) final;

    void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) final;

    void DispatchIndirect(RHIBuffer* indirectBuffer, uint32_t offset) final;

    void SetPushConstants(RHIPipeline* pipelineHandle, VectorView<uint8_t> data) final;

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

    void ChangeTextureLayout(RHITexture* texture, RHITextureLayout newLayout) final;

    void GenerateTextureMipmaps(RHITexture* texture) final;

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
} // namespace zen