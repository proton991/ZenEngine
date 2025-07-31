#pragma once
#include "RHICommon.h"
#include "Common/BitField.h"
#include "Common/SmallVector.h"

namespace zen::rhi
{
class RHICommandListContext
{
public:
    virtual ~RHICommandListContext() = default;
};

class RHICommandList
{
public:
    static RHICommandList* Create(GraphicsAPIType type, RHICommandListContext* context);

    virtual void BeginRender() = 0;

    virtual void EndRender() = 0;

    virtual void BeginUpload() = 0;

    virtual void EndUpload() = 0;

    virtual ~RHICommandList() = default;

    virtual void AddPipelineBarrier(BitField<PipelineStageBits> srcStages,
                                    BitField<PipelineStageBits> dstStages,
                                    const std::vector<MemoryTransition>& memoryTransitions,
                                    const std::vector<BufferTransition>& bufferTransitions,
                                    const std::vector<TextureTransition>& textureTransitions) = 0;

    virtual void ClearBuffer(BufferHandle bufferHandle, uint32_t offset, uint32_t size) = 0;

    virtual void CopyBuffer(BufferHandle srcBufferHandle,
                            BufferHandle dstBufferHandle,
                            const BufferCopyRegion& region) = 0;

    virtual void ClearTexture(TextureHandle textureHandle,
                              const Color& color,
                              const TextureSubResourceRange& range) = 0;

    virtual void CopyTexture(TextureHandle srcTextureHandle,
                             TextureHandle dstTextureHandle,
                             VectorView<TextureCopyRegion> regions) = 0;

    virtual void CopyTextureToBuffer(TextureHandle textureHandle,
                                     BufferHandle bufferHandle,
                                     VectorView<BufferTextureCopyRegion> regions) = 0;

    virtual void CopyBufferToTexture(BufferHandle bufferHandle,
                                     TextureHandle textureHandle,
                                     VectorView<BufferTextureCopyRegion> regions) = 0;

    virtual void ResolveTexture(TextureHandle srcTextureHandle,
                                TextureHandle dstTextureHandle,
                                uint32_t srcLayer,
                                uint32_t srcMipmap,
                                uint32_t dstLayer,
                                uint32_t dstMipmap) = 0;

    virtual void BindIndexBuffer(BufferHandle bufferHandle, DataFormat format, uint32_t offset) = 0;

    virtual void BindVertexBuffers(VectorView<BufferHandle> bufferHandles,
                                   const uint64_t* offsets) = 0;

    virtual void BindGfxPipeline(PipelineHandle pipelineHandle) = 0;

    virtual void BindComputePipeline(PipelineHandle pipelineHandle) = 0;

    virtual void BeginRenderPass(RenderPassHandle renderPassHandle,
                                 FramebufferHandle framebufferHandle,
                                 const Rect2<int>& area,
                                 VectorView<RenderPassClearValue> clearValues) = 0;

    virtual void EndRenderPass() = 0;

    virtual void BeginRenderPassDynamic(const RenderPassLayout& rpLayout,
                                        const Rect2<int>& area,
                                        VectorView<RenderPassClearValue> clearValues) = 0;

    virtual void EndRenderPassDynamic() = 0;


    virtual void Draw(uint32_t vertexCount,
                      uint32_t instanceCount,
                      uint32_t firstVertex,
                      uint32_t firstInstance) = 0;

    virtual void DrawIndexed(uint32_t indexCount,
                             uint32_t instanceCount,
                             uint32_t firstIndex,
                             uint32_t vertexOffset,
                             uint32_t firstInstance) = 0;

    virtual void DrawIndexedIndirect(BufferHandle indirectBuffer,
                                     uint32_t offset,
                                     uint32_t drawCount,
                                     uint32_t stride) = 0;

    virtual void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) = 0;

    virtual void DispatchIndirect(BufferHandle indirectBuffer, uint32_t offset) = 0;

    virtual void SetPushConstants(PipelineHandle pipelineHandle, VectorView<uint8_t> data) = 0;

    virtual void SetViewports(VectorView<Rect2<float>> viewports) = 0;

    virtual void SetScissors(VectorView<Rect2<int>> scissors) = 0;

    virtual void SetDepthBias(float depthBiasConstantFactor,
                              float depthBiasClamp,
                              float depthBiasSlopeFactor) = 0;

    virtual void SetLineWidth(float width) = 0;

    virtual void SetBlendConstants(const Color& color) = 0;

    virtual void GenerateTextureMipmaps(TextureHandle textureHandle) = 0;

    virtual void ChangeTextureLayout(TextureHandle textureHandle, TextureLayout newLayout) = 0;
};
} // namespace zen::rhi