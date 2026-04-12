#pragma once
// #include "RHICommon.h"
#include "RHIResource.h"
#include "Memory/Memory.h"
#include "Templates/HeapVector.h"

#define ALLOC_CMD(...) new (AllocateCmd(sizeof(__VA_ARGS__), alignof(__VA_ARGS__))) __VA_ARGS__

namespace zen
{
class RHIDescriptorSet;
class RHIPipeline;

struct RHICommandBase
{
    RHICommandBase* pNextCmd{nullptr};

    virtual ~RHICommandBase() {}
};

enum class RHICommandContextType : uint32_t
{
    eGraphics     = 0,
    eAsyncCompute = 1,
    eTransfer     = 2,
    eMax          = 3
};

class IRHICommandContext
{
public:
    virtual RHICommandContextType GetContextType() = 0;

    virtual ~IRHICommandContext() {}

    virtual void RHIBeginRendering(const RHIRenderingLayout* pRenderingLayout) = 0;

    virtual void RHIEndRendering() = 0;

    virtual void RHISetScissor(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY) = 0;

    virtual void RHISetViewport(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY) = 0;

    virtual void RHISetDepthBias(float depthBiasConstantFactor,
                                 float depthBiasClamp,
                                 float depthBiasSlopeFactor) = 0;

    virtual void RHISetLineWidth(float lineWidth) = 0;

    virtual void RHISetBlendConstants(const Color& blendConstants) = 0;

    virtual void RHIBindPipeline(RHIPipeline* pPipeline,
                                 uint32_t numDescriptorSets,
                                 RHIDescriptorSet* const* pDescriptorSets) = 0;

    virtual void RHIBindVertexBuffers(VectorView<RHIBuffer*> pBuffers,
                                      VectorView<uint64_t> offsets) = 0;

    virtual void RHIBindVertexBuffer(RHIBuffer* pBuffer, uint64_t offset) = 0;

    virtual void RHIDraw(uint32_t vertexCount,
                         uint32_t instanceCount,
                         uint32_t firstVertex,
                         uint32_t firstInstance) = 0;

    virtual void RHIDrawIndexed(RHIBuffer* pIndexBuffer,
                                DataFormat indexFormat,
                                uint32_t indexBufferOffset,
                                uint32_t indexCount,
                                uint32_t instanceCount,
                                uint32_t firstIndex,
                                int32_t vertexOffset,
                                uint32_t firstInstance) = 0;

    virtual void RHIDrawIndexedIndirect(RHIBuffer* pIndirectBuffer,
                                        RHIBuffer* pIndexBuffer,
                                        DataFormat indexFormat,
                                        uint32_t indexBufferOffset,
                                        uint32_t offset,
                                        uint32_t drawCount,
                                        uint32_t stride) = 0;

    virtual void RHIDispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) = 0;

    virtual void RHIDispatchIndirect(RHIBuffer* pIndirectBuffer, uint32_t offset) = 0;

    virtual void RHISetPushConstants(RHIPipeline* pPipeline, VectorView<uint8_t> data) = 0;

    virtual void RHIAddTransitions(BitField<RHIPipelineStageBits> srcStages,
                                   BitField<RHIPipelineStageBits> dstStages,
                                   VectorView<RHIMemoryTransition> memoryTransitions,
                                   VectorView<RHIBufferTransition> bufferTransitions,
                                   VectorView<RHITextureTransition> textureTransitions) = 0;

    virtual void RHIGenTextureMipmaps(RHITexture* pTexture) = 0;

    virtual void RHIAddTextureTransition(RHITexture* pTexture, RHITextureLayout newLayout) = 0;

    virtual void RHIClearBuffer(RHIBuffer* pBuffer, uint32_t offset, uint32_t size) = 0;

    virtual void RHICopyBuffer(RHIBuffer* pSrcBuffer,
                               RHIBuffer* pDstBuffer,
                               const RHIBufferCopyRegion& region) = 0;

    virtual void RHIClearTexture(RHITexture* pTex,
                                 const Color& color,
                                 const RHITextureSubResourceRange& range) = 0;

    virtual void RHICopyTexture(RHITexture* pSrcTexture,
                                RHITexture* pDstTexture,
                                VectorView<RHITextureCopyRegion> regions) = 0;


    virtual void RHICopyTextureToBuffer(RHITexture* pSrcTex,
                                        RHIBuffer* pDstBuffer,
                                        VectorView<RHIBufferTextureCopyRegion> regions) = 0;


    virtual void RHICopyBufferToTexture(RHIBuffer* pSrcBuffer,
                                        RHITexture* pDstTexture,
                                        VectorView<RHIBufferTextureCopyRegion> regions) = 0;

    virtual void RHIResolveTexture(RHITexture* pSrcTexture,
                                   RHITexture* pDstTexture,
                                   uint32_t srcLayer,
                                   uint32_t srcMipmap,
                                   uint32_t dstLayer,
                                   uint32_t dstMipmap) = 0;

    virtual void RHIWaitUntilCompleted() = 0;
};

class RHICommandListBase
{
public:
    virtual ~RHICommandListBase()
    {
        Reset();
        m_ppCmdPtr = nullptr;

        if (!m_pGraphicsContext && m_pComputeContext)
        {
            ZEN_DELETE(m_pComputeContext);
            m_pComputeContext = nullptr;
        }

        if (m_pGraphicsContext)
        {
            ZEN_DELETE(m_pGraphicsContext);
            m_pGraphicsContext = nullptr;
        }
    }

    void* AllocateCmd(uint32_t size, uint32_t alignment)
    {
        RHICommandBase* pCmd = static_cast<RHICommandBase*>(ZEN_MEM_ALLOC_ALIGNED(size, alignment));
        *m_ppCmdPtr          = pCmd;
        m_ppCmdPtr           = &pCmd->pNextCmd;
        ++m_numCommands;
        return pCmd;
    }

    IRHICommandContext* GetContext() const
    {
        return m_pGraphicsContext != nullptr ? m_pGraphicsContext : m_pComputeContext;
    }

    void WaitUntilCompleted()
    {
        if (IRHICommandContext* pContext = GetContext())
        {
            pContext->RHIWaitUntilCompleted();
        }
    }

    void Execute();

    void Reset();

protected:
    RHICommandListBase()
    {
        m_ppCmdPtr = &m_pCmdHead;
    }

    RHICommandBase* m_pCmdHead{nullptr};
    RHICommandBase** m_ppCmdPtr{nullptr};

    IRHICommandContext* m_pGraphicsContext{nullptr};
    IRHICommandContext* m_pComputeContext{nullptr};

    uint32_t m_numCommands{0};
};

struct RHICommand : public RHICommandBase
{
    virtual ~RHICommand() {};

    virtual void Execute(RHICommandListBase& cmdList) = 0;
};

struct RHICommandClearBuffer : public RHICommand
{
    RHIBuffer* pBuffer;
    uint32_t offset;
    uint32_t size;

    RHICommandClearBuffer(RHIBuffer* pBuffer, uint32_t offset, uint32_t size) :
        pBuffer(pBuffer), offset(offset), size(size)
    {}

    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHIClearBuffer(pBuffer, offset, size);
    }
};

struct RHICommandCopyBuffer : public RHICommand
{
    RHIBuffer* pSrcBuffer;
    RHIBuffer* pDstBuffer;
    RHIBufferCopyRegion copyRegion;

    RHICommandCopyBuffer(RHIBuffer* pSrcBuffer,
                         RHIBuffer* pDstBuffer,
                         const RHIBufferCopyRegion& region) :
        pSrcBuffer(pSrcBuffer), pDstBuffer(pDstBuffer), copyRegion(region)
    {}

    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHICopyBuffer(pSrcBuffer, pDstBuffer, copyRegion);
    }
};

struct RHICommandClearTexture : public RHICommand
{
    RHITexture* pTexture;
    Color clearColor;
    RHITextureSubResourceRange range;

    RHICommandClearTexture(RHITexture* pTexture,
                           const Color& color,
                           const RHITextureSubResourceRange& range) :
        pTexture(pTexture), clearColor(color), range(range)
    {}

    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHIClearTexture(pTexture, clearColor, range);
    }
};

struct RHICommandCopyTexture : public RHICommand
{
    RHITexture* pSrcTexture;
    RHITexture* pDstTexture;
    VectorView<RHITextureCopyRegion> copyRegions;
    //uint32_t numCopyRegions;

    //PRIVATE_ARRAY_DEF(RHITextureCopyRegion, CopyRegions, &this[1])

    RHICommandCopyTexture(RHITexture* pSrcTexture, RHITexture* pDstTexture) :
        pSrcTexture(pSrcTexture), pDstTexture(pDstTexture)
    {}

    ~RHICommandCopyTexture() override
    {
        ZEN_MEM_FREE(copyRegions.data());
    }

    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHICopyTexture(pSrcTexture, pDstTexture, copyRegions);
    }
};

struct RHICommandCopyBufferToTexture : public RHICommand
{
    RHIBuffer* pSrcBuffer;
    RHITexture* pDstTexture;
    VectorView<RHIBufferTextureCopyRegion> copyRegions;
    //uint32_t numCopyRegions;

    //PRIVATE_ARRAY_DEF(RHIBufferTextureCopyRegion, CopyRegions, &this[1])

    RHICommandCopyBufferToTexture(RHIBuffer* pSrcBuffer, RHITexture* pDstTexture) :
        pSrcBuffer(pSrcBuffer), pDstTexture(pDstTexture)
    {}

    ~RHICommandCopyBufferToTexture() override
    {
        ZEN_MEM_FREE(copyRegions.data());
    }

    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHICopyBufferToTexture(pSrcBuffer, pDstTexture, copyRegions);
    }
};

struct RHICommandCopyTextureToBuffer : public RHICommand
{
    RHITexture* pSrcTexture;
    RHIBuffer* pDstBuffer;
    VectorView<RHIBufferTextureCopyRegion> copyRegions;

    RHICommandCopyTextureToBuffer(RHITexture* pSrcTexture, RHIBuffer* pDstBuffer) :
        pSrcTexture(pSrcTexture), pDstBuffer(pDstBuffer)
    {}

    ~RHICommandCopyTextureToBuffer() override
    {
        ZEN_MEM_FREE(copyRegions.data());
    }

    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHICopyTextureToBuffer(pSrcTexture, pDstBuffer, copyRegions);
    }
};

struct RHICommandResolveTexture : public RHICommand
{
    RHITexture* pSrcTexture;
    RHITexture* pDstTexture;
    uint32_t srcLayer;
    uint32_t srcMipmap;
    uint32_t dstLayer;
    uint32_t dstMipmap;

    RHICommandResolveTexture(RHITexture* pSrcTexture,
                             RHITexture* pDstTexture,
                             uint32_t srcLayer,
                             uint32_t srcMipmap,
                             uint32_t dstLayer,
                             uint32_t dstMipmap) :
        pSrcTexture(pSrcTexture),
        pDstTexture(pDstTexture),
        srcLayer(srcLayer),
        srcMipmap(srcMipmap),
        dstLayer(dstLayer),
        dstMipmap(dstMipmap)
    {}

    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHIResolveTexture(pSrcTexture, pDstTexture, srcLayer, srcMipmap,
                                                dstLayer, dstMipmap);
    }
};

struct RHICommandBeginRendering final : public RHICommand
{
    const RHIRenderingLayout* pRenderingLayout;

    explicit RHICommandBeginRendering(const RHIRenderingLayout* pRenderingLayout) :
        pRenderingLayout(pRenderingLayout)
    {}

    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHIBeginRendering(pRenderingLayout);
    }
};

struct RHICommandEndRendering final : public RHICommand
{
    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHIEndRendering();
    }
};

struct RHICommandBindPipeline final : public RHICommand
{
    RHIPipelineType pipelineType;
    RHIPipeline* pPipeline;
    uint32_t numDescriptorSets;
    RHIDescriptorSet* const* pDescriptorSets;

    explicit RHICommandBindPipeline(RHIPipelineType pipelineType,
                                    RHIPipeline* pPipeline,
                                    uint32_t numDescriptorSets,
                                    RHIDescriptorSet* const* pDescriptorSets) :
        pipelineType(pipelineType),
        pPipeline(pPipeline),
        numDescriptorSets(numDescriptorSets),
        pDescriptorSets(pDescriptorSets)
    {}

    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHIBindPipeline(pPipeline, numDescriptorSets, pDescriptorSets);
    }
};

struct RHICommandSetScissor final : public RHICommand
{
    uint32_t minX;
    uint32_t minY;
    uint32_t maxX;
    uint32_t maxY;

    RHICommandSetScissor(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY) :
        minX(minX), minY(minY), maxX(maxX), maxY(maxY)
    {}

    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHISetScissor(minX, minY, maxX, maxY);
    }
};

struct RHICommandSetViewport final : public RHICommand
{
    uint32_t minX;
    uint32_t minY;
    uint32_t maxX;
    uint32_t maxY;

    RHICommandSetViewport(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY) :
        minX(minX), minY(minY), maxX(maxX), maxY(maxY)
    {}

    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHISetViewport(minX, minY, maxX, maxY);
    }
};

struct RHICommandSetDepthBias : public RHICommand
{
    float depthBiasConstantFactor;
    float depthBiasClamp;
    float depthBiasSlopeFactor;

    RHICommandSetDepthBias(float depthBiasConstantFactor,
                           float depthBiasClamp,
                           float depthBiasSlopeFactor) :
        depthBiasConstantFactor(depthBiasConstantFactor),
        depthBiasClamp(depthBiasClamp),
        depthBiasSlopeFactor(depthBiasSlopeFactor)
    {}

    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHISetDepthBias(depthBiasConstantFactor, depthBiasClamp,
                                              depthBiasSlopeFactor);
    }
};

struct RHICommandSetLineWidth : public RHICommand
{
    float lineWidth;

    explicit RHICommandSetLineWidth(float lineWidth) : lineWidth(lineWidth) {}

    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHISetLineWidth(lineWidth);
    }
};

struct RHICommandSetBlendConstants : public RHICommand
{
    Color blendConstants;

    explicit RHICommandSetBlendConstants(Color blendConstants) :
        blendConstants(std::move(blendConstants))
    {}

    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHISetBlendConstants(blendConstants);
    }
};

struct RHICommandBindVertexBuffer final : public RHICommand
{
    RHIBuffer* pVertexBuffer;
    uint64_t offset;

    RHICommandBindVertexBuffer(RHIBuffer* pVertexBuffer, uint64_t offset) :
        pVertexBuffer(pVertexBuffer), offset(offset)
    {}

    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHIBindVertexBuffer(pVertexBuffer, offset);
    }
};

struct RHICommandBindVertexBuffers final : public RHICommand
{
    VectorView<RHIBuffer*> vertexBuffers;
    VectorView<uint64_t> offsets;

    RHICommandBindVertexBuffers() = default;

    ~RHICommandBindVertexBuffers() override
    {
        ZEN_MEM_FREE(vertexBuffers.data());
        ZEN_MEM_FREE(offsets.data());
    }

    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHIBindVertexBuffers(vertexBuffers, offsets);
    }
};

struct RHICommandDraw final : public RHICommand
{
    uint32_t vertexCount;
    uint32_t instanceCount;
    uint32_t firstVertex;
    uint32_t firstInstance;

    RHICommandDraw(uint32_t vertexCount,
                   uint32_t instanceCount,
                   uint32_t firstVertex,
                   uint32_t firstInstance) :
        vertexCount(vertexCount),
        instanceCount(instanceCount),
        firstVertex(firstVertex),
        firstInstance(firstInstance)
    {}

    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHIDraw(vertexCount, instanceCount, firstVertex, firstInstance);
    }
};

struct RHICommandDrawIndexed final : public RHICommand
{
    struct Param
    {
        RHIBuffer* pIndexBuffer;
        DataFormat indexFormat;
        uint32_t indexBufferOffset;
        uint32_t indexCount;
        uint32_t instanceCount;
        uint32_t firstIndex;
        int32_t vertexOffset;
        uint32_t firstInstance;
    };

    // note: For now index buffer format is UINT32 only
    RHIBuffer* pIndexBuffer;
    DataFormat indexFormat;
    uint32_t indexBufferOffset;
    uint32_t indexCount;
    uint32_t instanceCount;
    uint32_t firstIndex;
    int32_t vertexOffset;
    uint32_t firstInstance;

    explicit RHICommandDrawIndexed(const Param& param) :
        pIndexBuffer(param.pIndexBuffer),
        indexFormat(param.indexFormat),
        indexBufferOffset(param.indexBufferOffset),
        indexCount(param.indexCount),
        instanceCount(param.instanceCount),
        firstIndex(param.firstIndex),
        vertexOffset(param.vertexOffset),
        firstInstance(param.firstInstance)
    {}

    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHIDrawIndexed(pIndexBuffer, indexFormat, indexBufferOffset,
                                             indexCount, instanceCount, firstIndex, vertexOffset,
                                             firstInstance);
    }
};

struct RHICommandDrawIndexedIndirect final : public RHICommand
{
    struct Param
    {
        RHIBuffer* pIndirectBuffer;
        RHIBuffer* pIndexBuffer;
        DataFormat indexFormat;
        uint32_t indexBufferOffset;
        uint32_t offset;
        uint32_t drawCount;
        uint32_t stride;
    };

    RHIBuffer* pIndirectBuffer;
    RHIBuffer* pIndexBuffer;
    DataFormat indexFormat;
    uint32_t indexBufferOffset;
    uint32_t offset;
    uint32_t drawCount;
    uint32_t stride;

    explicit RHICommandDrawIndexedIndirect(const Param& param) :
        pIndirectBuffer(param.pIndirectBuffer),
        pIndexBuffer(param.pIndexBuffer),
        indexFormat(param.indexFormat),
        indexBufferOffset(param.indexBufferOffset),
        offset(param.offset),
        drawCount(param.drawCount),
        stride(param.stride)
    {}

    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHIDrawIndexedIndirect(pIndirectBuffer, pIndexBuffer, indexFormat,
                                                     indexBufferOffset, offset, drawCount, stride);
    }
};

struct RHICommandDispatch final : public RHICommand
{
    uint32_t groupCountX;
    uint32_t groupCountY;
    uint32_t groupCountZ;

    RHICommandDispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) :
        groupCountX(groupCountX), groupCountY(groupCountY), groupCountZ(groupCountZ)
    {}

    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHIDispatch(groupCountX, groupCountY, groupCountZ);
    }
};

struct RHICommandDispatchIndirect final : public RHICommand
{
    RHIBuffer* pIndirectBuffer;
    uint32_t offset;

    RHICommandDispatchIndirect(RHIBuffer* pIndirectBuffer, uint32_t offset) :
        pIndirectBuffer(pIndirectBuffer), offset(offset)
    {}

    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHIDispatchIndirect(pIndirectBuffer, offset);
    }
};

struct RHICommandSetPushConstants final : public RHICommand
{
    RHIPipeline* pPipeline;
    VectorView<uint8_t> data;

    explicit RHICommandSetPushConstants(RHIPipeline* pPipeline) : pPipeline(pPipeline) {}

    ~RHICommandSetPushConstants() override
    {
        ZEN_MEM_FREE(data.data());
    }

    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHISetPushConstants(pPipeline, data);
    }
};

struct RHICommandAddTransitions final : public RHICommand
{
    BitField<RHIPipelineStageBits> srcStages;
    BitField<RHIPipelineStageBits> dstStages;

    VectorView<RHIMemoryTransition> memoryTransitions;
    VectorView<RHIBufferTransition> bufferTransitions;
    VectorView<RHITextureTransition> textureTransitions;

    ~RHICommandAddTransitions() override
    {
        ZEN_MEM_FREE(memoryTransitions.data());
        ZEN_MEM_FREE(bufferTransitions.data());
        ZEN_MEM_FREE(textureTransitions.data());
    }

    //PRIVATE_ARRAY_DEF(RHIMemoryTransition, MemoryTransitions, &this[1])
    //PRIVATE_ARRAY_DEF(RHIBufferTransition,
    //                  BufferTransitions,
    //                  &MemoryTransitions()[numMemoryTransitions])
    //PRIVATE_ARRAY_DEF(RHITextureTransition,
    //                  TextureTransitions,
    //                  &BufferTransitions()[numBufferTransitions])

    RHICommandAddTransitions(BitField<RHIPipelineStageBits> srcStages,
                             BitField<RHIPipelineStageBits> dstStages) :
        srcStages(srcStages), dstStages(dstStages)
    {}

    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHIAddTransitions(srcStages, dstStages, memoryTransitions,
                                                bufferTransitions, textureTransitions);
    }
};

struct RHICommandAddTextureTransition : public RHICommand
{
    RHITexture* pTexture;

    RHITextureLayout newLayout;

    RHICommandAddTextureTransition(RHITexture* pTexture, RHITextureLayout newLayout) :
        pTexture(pTexture), newLayout(newLayout)
    {}

    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHIAddTextureTransition(pTexture, newLayout);
    }
};

struct RHICommandGenTextureMipmaps : public RHICommand
{
    RHITexture* pTexture;

    RHICommandGenTextureMipmaps(RHITexture* pTexture) : pTexture(pTexture) {}

    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHIGenTextureMipmaps(pTexture);
    }
};

// Recorded command-list API used by the active RenderCore/V2 path.
class RHICommandList : public RHICommandListBase
{
public:
    static RHICommandList* Create(IRHICommandContext* pContext);

    RHICommandList() = default;

    ~RHICommandList() override = default;

    void ClearBuffer(RHIBuffer* pBuffer, uint32_t offset, uint32_t size);

    void CopyBuffer(RHIBuffer* pSrcBuffer,
                    RHIBuffer* pDstBuffer,
                    const RHIBufferCopyRegion& region);

    void ClearTexture(RHITexture* pTexture,
                      const Color& color,
                      const RHITextureSubResourceRange& range);

    void CopyTexture(RHITexture* pSrcTextureHandle,
                     RHITexture* pDstTextureHandle,
                     VectorView<RHITextureCopyRegion> regions);

    void CopyTextureToBuffer(RHITexture* pSrcTex,
                             RHIBuffer* pDstBuffer,
                             VectorView<RHIBufferTextureCopyRegion> regions);

    void CopyBufferToTexture(RHIBuffer* pSrcBuffer,
                             RHITexture* pDstTexture,
                             VectorView<RHIBufferTextureCopyRegion> regions);

    void ResolveTexture(RHITexture* pSrcTexture,
                        RHITexture* pDstTexture,
                        uint32_t srcLayer,
                        uint32_t srcMipmap,
                        uint32_t dstLayer,
                        uint32_t dstMipmap);

    void SetViewport(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY);

    void SetScissor(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY);

    void SetDepthBias(float depthBiasConstantFactor,
                      float depthBiasClamp,
                      float depthBiasSlopeFactor);

    void SetLineWidth(float width);

    void SetBlendConstants(const Color& color);

    void BindPipeline(RHIPipelineType pipelineType,
                      RHIPipeline* pPipeline,
                      uint32_t numDescriptorSets,
                      RHIDescriptorSet* const* pDescriptorSets);

    void BeginRendering(const RHIRenderingLayout* pRenderingLayout);

    void EndRendering();

    void BindVertexBuffers(VectorView<RHIBuffer*> vertexBuffers, VectorView<uint64_t> offsets);

    // All vertex attributes packed in 1 buffer
    void BindVertexBuffer(RHIBuffer* pBuffer, uint64_t offset);

    void Draw(uint32_t vertexCount,
              uint32_t instanceCount,
              uint32_t firstVertex,
              uint32_t firstInstance);

    void DrawIndexed(const RHICommandDrawIndexed::Param& param);

    void DrawIndexedIndirect(const RHICommandDrawIndexedIndirect::Param& param);

    void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ);

    void DispatchIndirect(RHIBuffer* pIndirectBuffer, uint32_t offset);

    void SetPushConstants(RHIPipeline* pPipeline, VectorView<uint8_t> data);

    void AddTransitions(BitField<RHIPipelineStageBits> srcStages,
                        BitField<RHIPipelineStageBits> dstStages,
                        const HeapVector<RHIMemoryTransition>& memoryTransitions,
                        const HeapVector<RHIBufferTransition>& bufferTransitions,
                        const HeapVector<RHITextureTransition>& textureTransitions);

    void AddTextureTransition(RHITexture* pTexture, RHITextureLayout newLayout);

    void GenerateTextureMipmaps(RHITexture* pTexture);
    // todo: add BeginRendering/EndRendering && BeginRenderPass
};
} // namespace zen
