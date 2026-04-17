#include "Graphics/RHI/RHICommandList.h"
#include "Utils/Errors.h"

namespace zen
{
void RHICommandListBase::Execute()
{
    RHICommandBase* pCmd = m_pCmdHead;
    while (pCmd)
    {
        RHICommandBase* pNext = pCmd->pNextCmd; // Save next before freeing

        dynamic_cast<RHICommand*>(pCmd)->Execute(*this);

        pCmd = pNext;
    }
}

void RHICommandListBase::Reset()
{
    RHICommandBase* pCmd = m_pCmdHead;
    while (pCmd)
    {
        RHICommandBase* pNext = pCmd->pNextCmd;

        pCmd->~RHICommandBase();
        ZEN_MEM_FREE(pCmd);

        pCmd = pNext;
    }

    m_pCmdHead    = nullptr;
    m_ppCmdPtr    = &m_pCmdHead;
    m_numCommands = 0;
}

RHICommandList* RHICommandList::Create(IRHICommandContext* pContext)
{
    RHICommandList* pCmdList          = ZEN_NEW() RHICommandList();
    RHICommandContextType contextType = pContext->GetContextType();
    if (contextType == RHICommandContextType::eGraphics ||
        contextType == RHICommandContextType::eTransfer)
    {
        pCmdList->m_pGraphicsContext = pContext;
        pCmdList->m_pComputeContext  = pContext;
    }
    else if (contextType == RHICommandContextType::eAsyncCompute)
    {
        pCmdList->m_pGraphicsContext = nullptr;
        pCmdList->m_pComputeContext  = pContext;
    }

    return pCmdList;
}

void RHICommandList::ClearBuffer(RHIBuffer* pBuffer, uint32_t offset, uint32_t size)
{
    ALLOC_CMD(RHICommandClearBuffer)(pBuffer, offset, size);
}

void RHICommandList::CopyBuffer(RHIBuffer* pSrcBuffer,
                                RHIBuffer* pDstBuffer,
                                const RHIBufferCopyRegion& region)
{
    ALLOC_CMD(RHICommandCopyBuffer)(pSrcBuffer, pDstBuffer, region);
}

void RHICommandList::ClearTexture(RHITexture* pTexture,
                                  const Color& color,
                                  const RHITextureSubResourceRange& range)
{
    ALLOC_CMD(RHICommandClearTexture)(pTexture, color, range);
}

void RHICommandList::CopyTexture(RHITexture* pSrcTexture,
                                 RHITexture* pDstTexture,
                                 VectorView<RHITextureCopyRegion> regions)
{
    RHICommandCopyTexture* pCmd = ALLOC_CMD(RHICommandCopyTexture)(pSrcTexture, pDstTexture);

    RHITextureCopyRegion* pRegions = static_cast<RHITextureCopyRegion*>(
        ZEN_MEM_ALLOC(sizeof(RHITextureCopyRegion) * regions.size()));
    std::ranges::copy(regions, pRegions);

    pCmd->copyRegions = MakeVecView(pRegions, regions.size());
    //RHITextureCopyRegion* pRegions = pCmd->copyRegions;
    //for (uint32_t i = 0; i < regions.size(); i++)
    //{
    //    pRegions[i] = regions[i];
    //}
}

void RHICommandList::CopyTextureToBuffer(RHITexture* pSrcTex,
                                         RHIBuffer* pDstBuffer,
                                         VectorView<RHIBufferTextureCopyRegion> regions)
{
    RHICommandCopyTextureToBuffer* pCmd =
        ALLOC_CMD(RHICommandCopyTextureToBuffer)(pSrcTex, pDstBuffer);

    RHIBufferTextureCopyRegion* pRegions = static_cast<RHIBufferTextureCopyRegion*>(
        ZEN_MEM_ALLOC(sizeof(RHIBufferTextureCopyRegion) * regions.size()));
    std::ranges::copy(regions, pRegions);

    pCmd->copyRegions = MakeVecView(pRegions, regions.size());
}

void RHICommandList::CopyBufferToTexture(RHIBuffer* pSrcBuffer,
                                         RHITexture* pDstTexture,
                                         VectorView<RHIBufferTextureCopyRegion> regions)
{
    RHICommandCopyBufferToTexture* pCmd =
        ALLOC_CMD(RHICommandCopyBufferToTexture)(pSrcBuffer, pDstTexture);

    RHIBufferTextureCopyRegion* pRegions = static_cast<RHIBufferTextureCopyRegion*>(
        ZEN_MEM_ALLOC(sizeof(RHIBufferTextureCopyRegion) * regions.size()));
    std::ranges::copy(regions, pRegions);

    pCmd->copyRegions = MakeVecView(pRegions, regions.size());
    //RHIBufferTextureCopyRegion* pRegions = pCmd->CopyRegions();
    //for (uint32_t i = 0; i < regions.size(); i++)
    //{
    //    pRegions[i] = regions[i];
    //}
}

void RHICommandList::ResolveTexture(RHITexture* pSrcTexture,
                                    RHITexture* pDstTexture,
                                    uint32_t srcLayer,
                                    uint32_t srcMipmap,
                                    uint32_t dstLayer,
                                    uint32_t dstMipmap)
{
    ALLOC_CMD(RHICommandResolveTexture)(pSrcTexture, pDstTexture, srcLayer, srcMipmap, dstLayer,
                                        dstMipmap);
}

void RHICommandList::SetViewport(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY)
{
    ALLOC_CMD(RHICommandSetViewport)(minX, minY, maxX, maxY);
}

void RHICommandList::SetScissor(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY)
{
    ALLOC_CMD(RHICommandSetScissor)(minX, minY, maxX, maxY);
}

void RHICommandList::SetDepthBias(float depthBiasConstantFactor,
                                  float depthBiasClamp,
                                  float depthBiasSlopeFactor)
{
    ALLOC_CMD(RHICommandSetDepthBias)(depthBiasConstantFactor, depthBiasClamp,
                                      depthBiasSlopeFactor);
}

void RHICommandList::SetLineWidth(float width)
{
    ALLOC_CMD(RHICommandSetLineWidth)(width);
}

void RHICommandList::SetBlendConstants(const Color& color)
{
    ALLOC_CMD(RHICommandSetBlendConstants)(color);
}

void RHICommandList::BeginRendering(const RHIRenderingLayout* pRenderingLayout)
{
    ALLOC_CMD(RHICommandBeginRendering)(pRenderingLayout);
}

void RHICommandList::EndRendering()
{
    ALLOC_CMD(RHICommandEndRendering)();
}

void RHICommandList::FlushCommands()
{
    ALLOC_CMD(RHICommandFlushCommands)();
}

void RHICommandList::BindPipeline(RHIPipelineType pipelineType,
                                  RHIPipeline* pPipeline,
                                  uint32_t numDescriptorSets,
                                  RHIDescriptorSet* const* pDescriptorSets)
{
    ALLOC_CMD(RHICommandBindPipeline)(pipelineType, pPipeline, numDescriptorSets, pDescriptorSets);
}

void RHICommandList::BindVertexBuffers(VectorView<RHIBuffer*> vertexBuffers,
                                       VectorView<uint64_t> offsets)
{
    VERIFY_EXPR(vertexBuffers.size() == offsets.size());

    RHICommandBindVertexBuffers* pCmd = ALLOC_CMD(RHICommandBindVertexBuffers)();

    RHIBuffer** ppVertexBuffers =
        static_cast<RHIBuffer**>(ZEN_MEM_ALLOC(sizeof(RHIBuffer*) * vertexBuffers.size()));
    std::ranges::copy(vertexBuffers, ppVertexBuffers);

    uint64_t* pOffsets = static_cast<uint64_t*>(ZEN_MEM_ALLOC(sizeof(uint64_t) * offsets.size()));
    std::ranges::copy(offsets, pOffsets);

    pCmd->vertexBuffers = MakeVecView(ppVertexBuffers, vertexBuffers.size());
    pCmd->offsets       = MakeVecView(pOffsets, offsets.size());
}

void RHICommandList::BindVertexBuffer(RHIBuffer* pBuffer, uint64_t offset)
{
    ALLOC_CMD(RHICommandBindVertexBuffer)(pBuffer, offset);
}

void RHICommandList::Draw(uint32_t vertexCount,
                          uint32_t instanceCount,
                          uint32_t firstVertex,
                          uint32_t firstInstance)
{
    ALLOC_CMD(RHICommandDraw)(vertexCount, instanceCount, firstVertex, firstInstance);
}

void RHICommandList::DrawIndexed(const RHICommandDrawIndexed::Param& param)
{
    ALLOC_CMD(RHICommandDrawIndexed)(param);
}

void RHICommandList::DrawIndexedIndirect(const RHICommandDrawIndexedIndirect::Param& param)
{
    ALLOC_CMD(RHICommandDrawIndexedIndirect)(param);
}

void RHICommandList::Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{
    ALLOC_CMD(RHICommandDispatch)(groupCountX, groupCountY, groupCountZ);
}

void RHICommandList::DispatchIndirect(RHIBuffer* pIndirectBuffer, uint32_t offset)
{
    ALLOC_CMD(RHICommandDispatchIndirect)(pIndirectBuffer, offset);
}

void RHICommandList::SetPushConstants(RHIPipeline* pPipeline, VectorView<uint8_t> data)
{
    RHICommandSetPushConstants* pCmd = ALLOC_CMD(RHICommandSetPushConstants)(pPipeline);

    uint8_t* pData = static_cast<uint8_t*>(ZEN_MEM_ALLOC(sizeof(uint8_t) * data.size()));
    std::ranges::copy(data, pData);
    pCmd->data = MakeVecView(pData, data.size());
}

void RHICommandList::AddTransitions(BitField<RHIPipelineStageBits> srcStages,
                                    BitField<RHIPipelineStageBits> dstStages,
                                    const HeapVector<RHIMemoryTransition>& memoryTransitions,
                                    const HeapVector<RHIBufferTransition>& bufferTransitions,
                                    const HeapVector<RHITextureTransition>& textureTransitions)
{
    RHICommandAddTransitions* pCmd = ALLOC_CMD(RHICommandAddTransitions)(srcStages, dstStages);

    RHIMemoryTransition* pMemoryTransitions = static_cast<RHIMemoryTransition*>(
        ZEN_MEM_ALLOC(sizeof(RHIMemoryTransition) * memoryTransitions.size()));
    std::ranges::copy(memoryTransitions, pMemoryTransitions);

    RHIBufferTransition* pBufferTransitions = static_cast<RHIBufferTransition*>(
        ZEN_MEM_ALLOC(sizeof(RHIBufferTransition) * bufferTransitions.size()));
    std::ranges::copy(bufferTransitions, pBufferTransitions);

    RHITextureTransition* pTextureTransitions = static_cast<RHITextureTransition*>(
        ZEN_MEM_ALLOC(sizeof(RHITextureTransition) * textureTransitions.size()));
    std::ranges::copy(textureTransitions, pTextureTransitions);

    pCmd->memoryTransitions  = MakeVecView(pMemoryTransitions, memoryTransitions.size());
    pCmd->bufferTransitions  = MakeVecView(pBufferTransitions, bufferTransitions.size());
    pCmd->textureTransitions = MakeVecView(pTextureTransitions, textureTransitions.size());

    //for (uint32_t i = 0; i < numMemoryTransitions; ++i)
    //{
    //    pMemoryTransitions[i] = memoryTransitions[i];
    //}

    //for (uint32_t i = 0; i < numBufferTransitions; ++i)
    //{
    //    pBufferTransitions[i] = bufferTransitions[i];
    //}

    //for (uint32_t i = 0; i < numTextureTransitions; ++i)
    //{
    //    pTextureTransitions[i] = textureTransitions[i];
    //}
}

void RHICommandList::AddTextureTransition(RHITexture* pTexture, RHITextureLayout newLayout)
{
    ALLOC_CMD(RHICommandAddTextureTransition)(pTexture, newLayout);
}

void RHICommandList::GenerateTextureMipmaps(RHITexture* pTexture)
{
    ALLOC_CMD(RHICommandGenTextureMipmaps)(pTexture);
}
} // namespace zen
