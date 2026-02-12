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
};

enum class RHICommandContextType : uint32_t
{
    eGraphics,
    eAsyncCompute,
    eTransfer
};

class IRHICommandContext
{
public:
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

    virtual void RHIBindVertexBuffer(RHIBuffer* pBuffer, uint64_t offset) = 0;

    virtual void RHIDrawIndexed(RHIBuffer* pIndexBuffer,
                                uint32_t indexCount,
                                uint32_t instanceCount,
                                uint32_t firstIndex,
                                int32_t vertexOffset,
                                uint32_t firstInstance) = 0;

    virtual void RHIDrawIndexedIndirect(RHIBuffer* pIndirectBuffer,
                                        RHIBuffer* pIndexBuffer,
                                        uint32_t offset,
                                        uint32_t drawCount,
                                        uint32_t stride) = 0;

    virtual void RHIDispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) = 0;

    virtual void RHIAddTransitions(BitField<RHIPipelineStageBits> srcStages,
                                   BitField<RHIPipelineStageBits> dstStages,
                                   const HeapVector<RHIMemoryTransition>& memoryTransitions,
                                   const HeapVector<RHIBufferTransition>& bufferTransitions,
                                   const HeapVector<RHITextureTransition>& textureTransitions) = 0;

    virtual void RHIDispatchIndirect(RHIBuffer* pIndirectBuffer, uint32_t offset) = 0;

    virtual void RHICopyBufferToTexture(RHIBuffer* pSrcBuffer,
                                        RHITexture* pDstTexture,
                                        uint32_t numRegions,
                                        RHIBufferTextureCopyRegion* pRegions) = 0;

    virtual void RHIGenTextureMipmaps(RHITexture* pTexture) = 0;

    virtual void RHIAddTextureTransition(RHITexture* pTexture, RHITextureLayout newLayout) = 0;
};

class RHICommandListBase
{
public:
    virtual ~RHICommandListBase()
    {
        RHICommandBase* pCmd = m_pCmdHead;
        while (pCmd)
        {
            RHICommandBase* next = pCmd->pNextCmd; // Save next before freeing
            ZEN_MEM_FREE(pCmd);
            pCmd = next;
        }

        m_pCmdHead         = nullptr;
        m_ppCmdPtr         = nullptr;
        m_numCommands      = 0;
        m_pGraphicsContext = nullptr;
        m_pComputeContext  = nullptr;
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
        return m_pGraphicsContext;
    }

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

struct RHICommandCopyBufferToTexture : public RHICommand
{
    RHIBuffer* pSrcBuffer;
    RHITexture* pDstTexture;
    uint32_t numCopyRegions;

    PRIVATE_ARRAY_DEF(RHIBufferTextureCopyRegion, CopyRegions, &this[1])

    RHICommandCopyBufferToTexture(RHIBuffer* pSrcBuffer,
                                  RHITexture* pDstTexture,
                                  uint32_t numCopyRegions) :
        pSrcBuffer(pSrcBuffer), pDstTexture(pDstTexture), numCopyRegions(numCopyRegions)
    {}

    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHICopyBufferToTexture(pSrcBuffer, pDstTexture, numCopyRegions,
                                                     CopyRegions());
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

struct RHICommandDrawIndexed final : public RHICommand
{
    struct Param
    {
        RHIBuffer* pIndexBuffer;
        uint32_t offset;
        uint32_t indexCount;
        uint32_t instanceCount;
        uint32_t firstIndex;
        int32_t vertexOffset;
        uint32_t firstInstance;
    };

    // note: For now index buffer format is UINT32 only
    RHIBuffer* pIndexBuffer;
    uint32_t offset;
    uint32_t indexCount;
    uint32_t instanceCount;
    uint32_t firstIndex;
    int32_t vertexOffset;
    uint32_t firstInstance;

    explicit RHICommandDrawIndexed(const Param& param) :
        pIndexBuffer(param.pIndexBuffer),
        offset(param.offset),
        indexCount(param.indexCount),
        instanceCount(param.instanceCount),
        firstIndex(param.firstIndex),
        vertexOffset(param.vertexOffset),
        firstInstance(param.firstInstance)
    {}

    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHIDrawIndexed(pIndexBuffer, indexCount, instanceCount, firstIndex,
                                             vertexOffset, firstInstance);
    }
};

struct RHICommandDrawIndexedIndirect final : public RHICommand
{
    struct Param
    {
        RHIBuffer* pIndirectBuffer;
        RHIBuffer* pIndexBuffer;
        uint32_t offset;
        uint32_t drawCount;
        uint32_t stride;
    };

    RHIBuffer* pIndirectBuffer;
    RHIBuffer* pIndexBuffer;
    uint32_t offset;
    uint32_t drawCount;
    uint32_t stride;

    explicit RHICommandDrawIndexedIndirect(const Param& param) :
        pIndirectBuffer(param.pIndirectBuffer),
        pIndexBuffer(param.pIndexBuffer),
        offset(param.offset),
        drawCount(param.drawCount),
        stride(param.stride)
    {}

    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHIDrawIndexedIndirect(pIndirectBuffer, pIndexBuffer, offset,
                                                     drawCount, stride);
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

struct RHICommandAddTransitions final : public RHICommand
{
    BitField<RHIPipelineStageBits> srcStages;
    BitField<RHIPipelineStageBits> dstStages;
    uint32_t numMemoryTransitions;
    uint32_t numBufferTransitions;
    uint32_t numTextureTransitions;
    PRIVATE_ARRAY_DEF(RHIMemoryTransition, MemoryTransitions, &this[1])
    PRIVATE_ARRAY_DEF(RHIBufferTransition,
                      BufferTransitions,
                      &MemoryTransitions()[numMemoryTransitions])
    PRIVATE_ARRAY_DEF(RHITextureTransition,
                      TextureTransitions,
                      &BufferTransitions()[numBufferTransitions])

    RHICommandAddTransitions(BitField<RHIPipelineStageBits> srcStages,
                             BitField<RHIPipelineStageBits> dstStages,
                             uint32_t numMemoryTransitions,
                             uint32_t numBufferTransitions,
                             uint32_t numTextureTransitions) :
        srcStages(srcStages),
        dstStages(dstStages),
        numMemoryTransitions(numMemoryTransitions),
        numBufferTransitions(numBufferTransitions),
        numTextureTransitions(numTextureTransitions)
    {}

    void Execute(RHICommandListBase& cmdList) override
    {
        cmdList.GetContext()->RHIAddTransitions(srcStages, dstStages,
                                                {MemoryTransitions(), numMemoryTransitions},
                                                {BufferTransitions(), numBufferTransitions},
                                                {TextureTransitions(), numTextureTransitions});
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

class FRHICommandList : public RHICommandListBase
{
public:
    virtual void CopyBufferToTexture(RHIBuffer* pSrcBuffer,
                                     RHITexture* pDstTexture,
                                     VectorView<RHIBufferTextureCopyRegion> regions);

    virtual void SetViewport(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY);

    virtual void SetScissor(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY);

    virtual void BindPipeline(RHIPipelineType pipelineType,
                              RHIPipeline* pPipeline,
                              uint32_t numDescriptorSets,
                              RHIDescriptorSet* const* pDescriptorSets);

    // All vertex attributes packed in 1 buffer
    virtual void BindVertexBuffer(RHIBuffer* pBuffer, uint64_t offset);

    virtual void DrawIndexed(const RHICommandDrawIndexed::Param& param);

    virtual void DrawIndexedIndirect(const RHICommandDrawIndexedIndirect::Param& param);

    virtual void AddTransitions(BitField<RHIPipelineStageBits> srcStages,
                                BitField<RHIPipelineStageBits> dstStages,
                                const HeapVector<RHIMemoryTransition>& memoryTransitions,
                                const HeapVector<RHIBufferTransition>& bufferTransitions,
                                const HeapVector<RHITextureTransition>& textureTransitions);

    virtual void AddTextureTransition(RHITexture* pTexture, RHITextureLayout newLayout);

    virtual void GenTextureMipmaps(RHITexture* pTexture);
};
} // namespace zen