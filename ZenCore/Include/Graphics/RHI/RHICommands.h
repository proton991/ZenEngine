#pragma once
#include "RHICommon.h"
#include "Templates/HeapVector.h"
#include "Templates/SmallVector.h"

namespace zen
{
// Legacy immediate command-list API. New code should use Graphics/RHI/RHICommandList.h.
class RHIBuffer;
class RHITexture;
class RHIPipeline;
class RHIDescriptorSet;
struct RHIRenderingLayout;

class LegacyRHICommandListContext
{
public:
    virtual ~LegacyRHICommandListContext() = default;
};

// todo: refactor this as follows:
// LegacyRHICommandList API do not call gfx API directly, instead, allocate command and store them.
// Move command implementation to RHICommandContext
// Implement other helper class to execute the command list
class LegacyRHICommandList
{
public:
    static LegacyRHICommandList* Create(RHIAPIType type, LegacyRHICommandListContext* pContext);

    virtual void BeginRenderWorkload() = 0;

    virtual void EndRenderWorkload() = 0;

    virtual void BeginTransferWorkload() = 0;

    virtual void EndTransferWorkload() = 0;

    virtual ~LegacyRHICommandList() = default;

    virtual void AddPipelineBarrier(BitField<RHIPipelineStageBits> srcStages,
                                    BitField<RHIPipelineStageBits> dstStages,
                                    const HeapVector<RHIMemoryTransition>& memoryTransitions,
                                    const HeapVector<RHIBufferTransition>& bufferTransitions,
                                    const HeapVector<RHITextureTransition>& textureTransitions) = 0;

    virtual void ClearBuffer(RHIBuffer* pBuffer, uint32_t offset, uint32_t size) = 0;

    virtual void CopyBuffer(RHIBuffer* pSrcBuffer,
                            RHIBuffer* pDstBuffer,
                            const RHIBufferCopyRegion& region) = 0;

    virtual void ClearTexture(RHITexture* pTextureHandle,
                              const Color& color,
                              const RHITextureSubResourceRange& range) = 0;

    virtual void CopyTexture(RHITexture* pSrcTextureHandle,
                             RHITexture* pDstTextureHandle,
                             VectorView<RHITextureCopyRegion> regions) = 0;

    virtual void CopyTextureToBuffer(RHITexture* pTextureHandle,
                                     RHIBuffer* pBuffer,
                                     VectorView<RHIBufferTextureCopyRegion> regions) = 0;

    virtual void CopyBufferToTexture(RHIBuffer* pBuffer,
                                     RHITexture* pTextureHandle,
                                     VectorView<RHIBufferTextureCopyRegion> regions) = 0;

    virtual void ResolveTexture(RHITexture* pSrcTextureHandle,
                                RHITexture* pDstTextureHandle,
                                uint32_t srcLayer,
                                uint32_t srcMipmap,
                                uint32_t dstLayer,
                                uint32_t dstMipmap) = 0;

    virtual void BindIndexBuffer(RHIBuffer* pBuffer, DataFormat format, uint32_t offset) = 0;

    virtual void BindVertexBuffers(VectorView<RHIBuffer*> buffers,
                                   VectorView<uint64_t> offsets) = 0;

    // virtual void BindGfxPipeline(PipelineHandle pipelineHandle) = 0;

    virtual void BindGfxPipeline(RHIPipeline* pPipelineHandle,
                                 uint32_t numDescriptorSets,
                                 RHIDescriptorSet* const* pDescriptorSets) = 0;

    // virtual void BindComputePipeline(PipelineHandle pipelineHandle) = 0;

    virtual void BindComputePipeline(RHIPipeline* pPipelineHandle,
                                     uint32_t numDescriptorSets,
                                     const RHIDescriptorSet* const* pDescriptorSets) = 0;

    // virtual void BeginRenderPass(RenderPassHandle renderPassHandle,
    //                              FramebufferHandle framebufferHandle,
    //                              const Rect2<int>& area,
    //                              VectorView<RHIRenderPassClearValue> clearValues) = 0;

    // virtual void EndRenderPass() = 0;

    // virtual void BeginRenderPassDynamic(const RHIRenderPassLayout& rpLayout,
    //                                     const Rect2<int>& area,
    //                                     VectorView<RHIRenderPassClearValue> clearValues) = 0;

    // virtual void EndRenderPassDynamic() = 0;

    virtual void BeginRendering(const RHIRenderingLayout* pRenderingLayout) = 0;

    virtual void EndRendering() = 0;


    virtual void Draw(uint32_t vertexCount,
                      uint32_t instanceCount,
                      uint32_t firstVertex,
                      uint32_t firstInstance) = 0;

    virtual void DrawIndexed(uint32_t indexCount,
                             uint32_t instanceCount,
                             uint32_t firstIndex,
                             int32_t vertexOffset,
                             uint32_t firstInstance) = 0;

    virtual void DrawIndexedIndirect(RHIBuffer* pIndirectBuffer,
                                     uint32_t offset,
                                     uint32_t drawCount,
                                     uint32_t stride) = 0;

    virtual void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) = 0;

    virtual void DispatchIndirect(RHIBuffer* pIndirectBuffer, uint32_t offset) = 0;

    virtual void SetPushConstants(RHIPipeline* pPipelineHandle, VectorView<uint8_t> data) = 0;

    virtual void SetViewports(VectorView<Rect2<float>> viewports) = 0;

    virtual void SetScissors(VectorView<Rect2<int>> scissors) = 0;

    virtual void SetDepthBias(float depthBiasConstantFactor,
                              float depthBiasClamp,
                              float depthBiasSlopeFactor) = 0;

    virtual void SetLineWidth(float width) = 0;

    virtual void SetBlendConstants(const Color& color) = 0;

    virtual void GenerateTextureMipmaps(RHITexture* pTextureHandle) = 0;

    virtual void AddTextureTransition(RHITexture* pTextureHandle, RHITextureLayout newLayout) = 0;
};
} // namespace zen
