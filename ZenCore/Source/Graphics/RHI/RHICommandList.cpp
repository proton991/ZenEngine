#include "Graphics/RHI/RHICommandList.h"

namespace zen
{
void FRHICommandList::CopyBufferToTexture(RHIBuffer* pSrcBuffer,
                                          RHITexture* pDstTexture,
                                          VectorView<RHIBufferTextureCopyRegion> regions)
{
    RHICommandCopyBufferToTexture* pCmd =
        ALLOC_CMD(RHICommandCopyBufferToTexture)(pSrcBuffer, pDstTexture, regions.size());
    RHIBufferTextureCopyRegion* pRegions = pCmd->CopyRegions();
    for (uint32_t i = 0; i < regions.size(); i++)
    {
        pRegions[i] = regions[i];
    }
}

void FRHICommandList::SetViewport(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY)
{
    ALLOC_CMD(RHICommandSetViewport)(minX, minY, maxX, maxY);
}

void FRHICommandList::SetScissor(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY)
{
    ALLOC_CMD(RHICommandSetScissor)(minX, minY, maxX, maxY);
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
                                     const std::vector<RHIMemoryTransition>& memoryTransitions,
                                     const std::vector<RHIBufferTransition>& bufferTransitions,
                                     const std::vector<RHITextureTransition>& textureTransitions)
{
    const uint32_t numMemoryTransitions  = memoryTransitions.size();
    const uint32_t numBufferTransitions  = bufferTransitions.size();
    const uint32_t numTextureTransitions = textureTransitions.size();

    RHICommandAddTransitions* pCmd = ALLOC_CMD(RHICommandAddTransitions)(
        srcStages, dstStages, numMemoryTransitions, numBufferTransitions, numTextureTransitions);

    RHIMemoryTransition* pMemoryTransitions   = pCmd->MemoryTransitions();
    RHIBufferTransition* pBufferTransitions   = pCmd->BufferTransitions();
    RHITextureTransition* pTextureTransitions = pCmd->TextureTransitions();

    for (uint32_t i = 0; i < numMemoryTransitions; ++i)
    {
        pMemoryTransitions[i] = memoryTransitions[i];
    }

    for (uint32_t i = 0; i < numBufferTransitions; ++i)
    {
        pBufferTransitions[i] = bufferTransitions[i];
    }

    for (uint32_t i = 0; i < numTextureTransitions; ++i)
    {
        pTextureTransitions[i] = textureTransitions[i];
    }
}

void FRHICommandList::AddTextureTransition(RHITexture* pTexture, RHITextureLayout newLayout)
{
    ALLOC_CMD(RHICommandAddTextureTransition)(pTexture, newLayout);
}

void FRHICommandList::GenTextureMipmaps(RHITexture* pTexture)
{
    ALLOC_CMD(RHICommandGenTextureMipmaps)(pTexture);
}
} // namespace zen