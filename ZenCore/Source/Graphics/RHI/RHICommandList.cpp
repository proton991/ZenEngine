#include "Graphics/RHI/RHICommandList.h"

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

FRHICommandList* FRHICommandList::Create(IRHICommandContext* pContext)
{
    FRHICommandList* pCmdList         = ZEN_NEW() FRHICommandList();
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

void FRHICommandList::ClearBuffer(RHIBuffer* pBuffer, uint32_t offset, uint32_t size)
{
    ALLOC_CMD(RHICommandClearBuffer)(pBuffer, offset, size);
}

void FRHICommandList::CopyBuffer(RHIBuffer* pSrcBuffer,
                                 RHIBuffer* pDstBuffer,
                                 const RHIBufferCopyRegion& region)
{
    ALLOC_CMD(RHICommandCopyBuffer)(pSrcBuffer, pDstBuffer, region);
}

void FRHICommandList::ClearTexture(RHITexture* pTexture,
                                   const Color& color,
                                   const RHITextureSubResourceRange& range)
{
    ALLOC_CMD(RHICommandClearTexture)(pTexture, color, range);
}

void FRHICommandList::CopyTexture(RHITexture* pSrcTexture,
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

void FRHICommandList::CopyTextureToBuffer(RHITexture* pSrcTex,
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

void FRHICommandList::CopyBufferToTexture(RHIBuffer* pSrcBuffer,
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

void FRHICommandList::ResolveTexture(RHITexture* srcTexture,
                                     RHITexture* dstTexture,
                                     uint32_t srcLayer,
                                     uint32_t srcMipmap,
                                     uint32_t dstLayer,
                                     uint32_t dstMipmap)
{
    ALLOC_CMD(RHICommandResolveTexture)(srcTexture, dstTexture, srcLayer, srcMipmap, dstLayer,
                                        dstMipmap);
}

void FRHICommandList::SetViewport(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY)
{
    ALLOC_CMD(RHICommandSetViewport)(minX, minY, maxX, maxY);
}

void FRHICommandList::SetScissor(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY)
{
    ALLOC_CMD(RHICommandSetScissor)(minX, minY, maxX, maxY);
}

void FRHICommandList::SetDepthBias(float depthBiasConstantFactor,
                                   float depthBiasClamp,
                                   float depthBiasSlopeFactor)
{
    ALLOC_CMD(RHICommandSetDepthBias)(depthBiasConstantFactor, depthBiasClamp,
                                      depthBiasSlopeFactor);
}

void FRHICommandList::SetLineWidth(float width)
{
    ALLOC_CMD(RHICommandSetLineWidth)(width);
}

void FRHICommandList::SetBlendConstants(const Color& color)
{
    ALLOC_CMD(RHICommandSetBlendConstants)(color);
}

void FRHICommandList::BindPipeline(RHIPipelineType pipelineType,
                                   RHIPipeline* pPipeline,
                                   uint32_t numDescriptorSets,
                                   RHIDescriptorSet* const* pDescriptorSets)
{
    ALLOC_CMD(RHICommandBindPipeline)(pipelineType, pPipeline, numDescriptorSets, pDescriptorSets);
}

void FRHICommandList::BindVertexBuffer(RHIBuffer* pBuffer, uint64_t offset)
{
    ALLOC_CMD(RHICommandBindVertexBuffer)(pBuffer, offset);
}

void FRHICommandList::DrawIndexed(const RHICommandDrawIndexed::Param& param)
{
    ALLOC_CMD(RHICommandDrawIndexed)(param);
}

void FRHICommandList::DrawIndexedIndirect(const RHICommandDrawIndexedIndirect::Param& param)
{
    ALLOC_CMD(RHICommandDrawIndexedIndirect)(param);
}

void FRHICommandList::AddTransitions(BitField<RHIPipelineStageBits> srcStages,
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
        ZEN_MEM_ALLOC(sizeof(RHIMemoryTransition) * bufferTransitions.size()));
    std::ranges::copy(bufferTransitions, pBufferTransitions);

    RHITextureTransition* pTextureTransitions = static_cast<RHITextureTransition*>(
        ZEN_MEM_ALLOC(sizeof(RHIMemoryTransition) * textureTransitions.size()));
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

void FRHICommandList::AddTextureTransition(RHITexture* pTexture, RHITextureLayout newLayout)
{
    ALLOC_CMD(RHICommandAddTextureTransition)(pTexture, newLayout);
}

void FRHICommandList::GenerateTextureMipmaps(RHITexture* pTexture)
{
    ALLOC_CMD(RHICommandGenTextureMipmaps)(pTexture);
}
} // namespace zen
