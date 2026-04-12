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

// Legacy Vulkan command-list implementation. Active RenderCore/V2 code uses
// FVulkanCommandListContext + RHICommandList instead.
class LegacyVulkanCommandListContext : public LegacyRHICommandListContext
{
public:
    explicit LegacyVulkanCommandListContext(VulkanRHI* pRHI);

    ~LegacyVulkanCommandListContext() override;

    auto* GetCmdBufferManager() const
    {
        return m_pCmdBufferMgr;
    }

    auto* GetVkRHI() const
    {
        return m_pVkRHI;
    }

private:
    VulkanRHI* m_pVkRHI{nullptr};
    VulkanCommandBufferManager* m_pCmdBufferMgr{nullptr};
};

class LegacyVulkanCommandList : public LegacyRHICommandList
{
public:
    explicit LegacyVulkanCommandList(LegacyVulkanCommandListContext* pContext);

    ~LegacyVulkanCommandList() override;

    void BeginRenderWorkload() final;
    void EndRenderWorkload() final;

    void BeginTransferWorkload() final;
    void EndTransferWorkload() final;

    void AddPipelineBarrier(BitField<RHIPipelineStageBits> srcStages,
                            BitField<RHIPipelineStageBits> dstStages,
                            const HeapVector<RHIMemoryTransition>& memoryTransitions,
                            const HeapVector<RHIBufferTransition>& bufferTransitions,
                            const HeapVector<RHITextureTransition>& textureTransitions) final;

    void ClearBuffer(RHIBuffer* pBuffer, uint32_t offset, uint32_t size) final;

    void CopyBuffer(RHIBuffer* pSrcBuffer,
                    RHIBuffer* pDstBuffer,
                    const RHIBufferCopyRegion& region) final;

    void ClearTexture(RHITexture* pTexture,
                      const Color& color,
                      const RHITextureSubResourceRange& range) final;

    void CopyTexture(RHITexture* pSrcTexture,
                     RHITexture* pDstTexture,
                     VectorView<RHITextureCopyRegion> regions) final;

    void CopyTextureToBuffer(RHITexture* pTexture,
                             RHIBuffer* pBuffer,
                             VectorView<RHIBufferTextureCopyRegion> regions) final;

    void CopyBufferToTexture(RHIBuffer* pBuffer,
                             RHITexture* pTexture,
                             VectorView<RHIBufferTextureCopyRegion> regions) final;

    void ResolveTexture(RHITexture* pSrcTexture,
                        RHITexture* pDstTexture,
                        uint32_t srcLayer,
                        uint32_t srcMipmap,
                        uint32_t dstLayer,
                        uint32_t dstMipmap) final;

    void BindIndexBuffer(RHIBuffer* pBuffer, DataFormat format, uint32_t offset) final;

    void BindVertexBuffers(VectorView<RHIBuffer*> buffers, VectorView<uint64_t> offsets) final;

    // void BindGfxPipeline(PipelineHandle pipelineHandle) final;

    void BindGfxPipeline(RHIPipeline* pPipelineHandle,
                         uint32_t numDescriptorSets,
                         RHIDescriptorSet* const* pDescriptorSets) final;

    // void BindComputePipeline(PipelineHandle pipelineHandle) final;

    void BindComputePipeline(RHIPipeline* pPipelineHandle,
                             uint32_t numDescriptorSets,
                             const RHIDescriptorSet* const* pDescriptorSets) final;

    // void BeginRenderPass(RenderPassHandle renderPassHandle,
    //                      FramebufferHandle framebuffer,
    //                      const Rect2<int>& area,
    //                      VectorView<RHIRenderPassClearValue> clearValues) final;

    // void EndRenderPass() final;

    // void BeginRenderPassDynamic(const RHIRenderPassLayout& rpLayout,
    //                             const Rect2<int>& area,
    //                             VectorView<RHIRenderPassClearValue> clearValues) final;

    // void EndRenderPassDynamic() final;

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

    void DrawIndexedIndirect(RHIBuffer* pIndirectBuffer,
                             uint32_t offset,
                             uint32_t drawCount,
                             uint32_t stride) final;

    void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) final;

    void DispatchIndirect(RHIBuffer* pIndirectBuffer, uint32_t offset) final;

    void SetPushConstants(RHIPipeline* pPipelineHandle, VectorView<uint8_t> data) final;

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
        return m_pCmdBufferManager;
    }

    void AddTextureTransition(RHITexture* pTexture, RHITextureLayout newLayout) final;

    void GenerateTextureMipmaps(RHITexture* pTexture) final;

    void ChangeImageLayout(VkImage image,
                           VkImageLayout srcLayout,
                           VkImageLayout dstLayout,
                           const VkImageSubresourceRange& range);

private:
    VulkanRHI* m_pVkRHI{nullptr};
    VulkanCommandBufferManager* m_pCmdBufferManager{nullptr};
    VulkanCommandBuffer* m_pCmdBuffer{nullptr};

    friend class VulkanRHI;
};
} // namespace zen
