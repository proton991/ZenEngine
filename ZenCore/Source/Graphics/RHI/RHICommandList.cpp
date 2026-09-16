#include "Graphics/RHI/RHICommandList.h"
#include "Utils/Errors.h"

namespace zen
{
RHIResourceReferences::~RHIResourceReferences()
{
    Reset();
}

RHIResourceReferences::RHIResourceReferences(RHIResourceReferences&& other) noexcept
{
    Swap(other);
}

RHIResourceReferences& RHIResourceReferences::operator=(RHIResourceReferences&& other) noexcept
{
    if (this != &other)
    {
        Reset();
        Swap(other);
    }
    return *this;
}

void RHIResourceReferences::Retain(RHIResource* resource)
{
    if (resource != nullptr && m_unique.try_emplace(resource, m_resources.size()).second)
    {
        m_resources.push_back(resource);
        resource->AddReference();
        if (resource->GetResourceType() == RHIResourceType::eTextureView)
        {
            Retain(static_cast<RHITextureView*>(resource)->GetTexture());
        }
    }
}

void RHIResourceReferences::Reset()
{
    Rollback(0);
}

size_t RHIResourceReferences::GetCount() const
{
    return m_resources.size();
}

void RHIResourceReferences::Rollback(size_t count)
{
    VERIFY_EXPR(count <= m_resources.size());
    for (size_t i = count; i < m_resources.size(); ++i)
    {
        RHIResource* resource = m_resources[i];
        m_unique.erase(resource);
        resource->ReleaseReference();
    }
    m_resources.resize(count);
}

void RHIResourceReferences::Swap(RHIResourceReferences& other)
{
    std::swap(m_resources, other.m_resources);
    m_unique.Swap(other.m_unique);
}

namespace
{
void DestroyCommandContext(IRHICommandContext* context)
{
    GetRHIThread().Invoke([context] { ZEN_DELETE(context); });
}
} // namespace

void RHICommandListDeleter::operator()(RHICommandList* commands) const
{
    ZEN_DELETE(commands);
}

RHICommandListPtr RHICommandList::DetachCommands(RHICommandListPtr reusable)
{
    RHICommandListPtr result = std::move(reusable);
    if (result == nullptr)
    {
        result.reset(ZEN_NEW() RHICommandList());
    }
    VERIFY_EXPR(result->GetCommandCount() == 0);
    result->m_contextOwner     = m_contextOwner;
    result->m_pGraphicsContext = m_pGraphicsContext;
    result->m_pComputeContext  = m_pComputeContext;
    result->m_cmdAllocator.Swap(m_cmdAllocator);
    result->m_resources.Swap(m_resources);
    result->m_pCmdHead    = m_pCmdHead;
    result->m_ppCmdPtr    = m_numCommands == 0 ? &result->m_pCmdHead : m_ppCmdPtr;
    result->m_numCommands = m_numCommands;
    m_pCmdHead            = nullptr;
    m_ppCmdPtr            = &m_pCmdHead;
    m_numCommands         = 0;
    return result;
}

void RHICommandList::ResetForReuse()
{
    GetRHIThread().CheckOwnership();
    Reset();
    m_contextOwner.reset();
    m_pGraphicsContext = nullptr;
    m_pComputeContext  = nullptr;
}

void RHICommandListBase::Execute()
{
    GetRHIThread().CheckOwnership();
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

        if (pCmd->pDestroy)
        {
            pCmd->pDestroy(pCmd);
        }
        else
        {
            pCmd->~RHICommandBase();
        }

        pCmd = pNext;
    }

    m_pCmdHead    = nullptr;
    m_ppCmdPtr    = &m_pCmdHead;
    m_numCommands = 0;
    m_cmdAllocator.Reset();
    m_resources.Reset();
}

void RHICommandListBase::RollbackCommands(CommandCheckpoint checkpoint)
{
    RHICommandBase* command = *checkpoint.tail;

    while (command != nullptr)
    {
        RHICommandBase* next = command->pNextCmd;

        if (command->pDestroy != nullptr)
        {
            command->pDestroy(command);
        }
        else
        {
            command->~RHICommandBase();
        }

        command = next;
    }

    *checkpoint.tail = nullptr;
    m_ppCmdPtr       = checkpoint.tail;
    m_numCommands    = checkpoint.count;
    m_resources.Rollback(checkpoint.resourceCount);
}

RHICommandList* RHICommandList::Create(IRHICommandContext* pContext)
{
    RHICommandList* pCmdList = ZEN_NEW() RHICommandList();
    pCmdList->m_contextOwner = std::shared_ptr<IRHICommandContext>(pContext, DestroyCommandContext);
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
    RetainResource(pBuffer);
    ALLOC_CMD(RHICommandClearBuffer)(pBuffer, offset, size);
}

void RHICommandList::CopyBuffer(RHIBuffer* pSrcBuffer,
                                RHIBuffer* pDstBuffer,
                                const RHIBufferCopyRegion& region)
{
    RetainResource(pSrcBuffer);
    RetainResource(pDstBuffer);
    ALLOC_CMD(RHICommandCopyBuffer)(pSrcBuffer, pDstBuffer, region);
}

void RHICommandList::ClearTexture(RHITexture* pTexture,
                                  const Color& color,
                                  const RHITextureSubResourceRange& range)
{
    RetainResource(pTexture);
    ALLOC_CMD(RHICommandClearTexture)(pTexture, color, range);
}

void RHICommandList::CopyTexture(RHITexture* pSrcTexture,
                                 RHITexture* pDstTexture,
                                 VectorView<const RHITextureCopyRegion> regions)
{
    RetainResource(pSrcTexture);
    RetainResource(pDstTexture);
    RHICommandCopyTexture* pCmd = ALLOC_CMD(RHICommandCopyTexture)(pSrcTexture, pDstTexture);

    RHITextureCopyRegion* pRegions = AllocateCmdData<RHITextureCopyRegion>(regions.size());

    if (pRegions != nullptr)
    {
        std::ranges::copy(regions, pRegions);
    }

    pCmd->copyRegions = MakeVecView(pRegions, regions.size());
    //RHITextureCopyRegion* pRegions = pCmd->copyRegions;
    //for (uint32_t i = 0; i < regions.size(); i++)
    //{
    //    pRegions[i] = regions[i];
    //}
}

void RHICommandList::BlitTexture(RHITexture* pSrcTexture,
                                 RHITexture* pDstTexture,
                                 VectorView<RHITextureBlitRegion> regions,
                                 RHISamplerFilter filter)
{
    RetainResource(pSrcTexture);
    RetainResource(pDstTexture);
    RHICommandBlitTexture* pCmd =
        ALLOC_CMD(RHICommandBlitTexture)(pSrcTexture, pDstTexture, filter);

    RHITextureBlitRegion* pRegions = AllocateCmdData<RHITextureBlitRegion>(regions.size());

    if (pRegions != nullptr)
    {
        std::ranges::copy(regions, pRegions);
    }

    pCmd->blitRegions = MakeVecView(pRegions, regions.size());
}

void RHICommandList::CopyTextureToBuffer(RHITexture* pSrcTex,
                                         RHIBuffer* pDstBuffer,
                                         VectorView<RHIBufferTextureCopyRegion> regions)
{
    RetainResource(pSrcTex);
    RetainResource(pDstBuffer);
    RHICommandCopyTextureToBuffer* pCmd =
        ALLOC_CMD(RHICommandCopyTextureToBuffer)(pSrcTex, pDstBuffer);

    RHIBufferTextureCopyRegion* pRegions =
        AllocateCmdData<RHIBufferTextureCopyRegion>(regions.size());

    if (pRegions != nullptr)
    {
        std::ranges::copy(regions, pRegions);
    }

    pCmd->copyRegions = MakeVecView(pRegions, regions.size());
}

void RHICommandList::CopyBufferToTexture(RHIBuffer* pSrcBuffer,
                                         RHITexture* pDstTexture,
                                         VectorView<const RHIBufferTextureCopyRegion> regions)
{
    RetainResource(pSrcBuffer);
    RetainResource(pDstTexture);
    RHICommandCopyBufferToTexture* pCmd =
        ALLOC_CMD(RHICommandCopyBufferToTexture)(pSrcBuffer, pDstTexture);

    RHIBufferTextureCopyRegion* pRegions =
        AllocateCmdData<RHIBufferTextureCopyRegion>(regions.size());

    if (pRegions != nullptr)
    {
        std::ranges::copy(regions, pRegions);
    }

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
    RetainResource(pSrcTexture);
    RetainResource(pDstTexture);
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
    VERIFY_EXPR(pRenderingLayout != nullptr);
    for (uint32_t i = 0; i < pRenderingLayout->numColorRenderTargets; ++i)
    {
        RetainResource(pRenderingLayout->colorRenderTargets[i].pTextureView);
        RetainResource(pRenderingLayout->colorRenderTargets[i].pTexture);
    }
    if (pRenderingLayout->hasDepthStencilRT)
    {
        RetainResource(pRenderingLayout->depthStencilRenderTarget.pTextureView);
        RetainResource(pRenderingLayout->depthStencilRenderTarget.pTexture);
    }
    ALLOC_CMD(RHICommandBeginRendering)(pRenderingLayout);
}

void RHICommandList::EndRendering()
{
    ALLOC_CMD(RHICommandEndRendering)();
}

void RHICommandList::BindPipeline(RHIPipelineType pipelineType, RHIPipeline* pPipeline)
{
    RetainResource(pPipeline);
    ALLOC_CMD(RHICommandBindPipeline)(pipelineType, pPipeline);
}

void RHICommandList::SetShaderParameters(const RHIBatchedShaderParameters& parameters)
{
    for (const RHIShaderResourceParameter& parameter : parameters.GetResourceParams())
    {
        RetainResource(parameter.pResource);
        RetainResource(parameter.pAuxResource);
    }
    for (const RHIShaderResourceParameter& parameter : parameters.GetBindlessParams())
    {
        RetainResource(parameter.pResource);
        RetainResource(parameter.pAuxResource);
    }
    ALLOC_CMD(RHICommandSetShaderParameters)(parameters, GetContext());
}

void RHICommandList::BindVertexBuffers(VectorView<RHIBuffer*> vertexBuffers,
                                       VectorView<uint64_t> offsets)
{
    for (RHIBuffer* buffer : vertexBuffers)
    {
        RetainResource(buffer);
    }
    VERIFY_EXPR(vertexBuffers.size() == offsets.size());

    RHICommandBindVertexBuffers* pCmd = ALLOC_CMD(RHICommandBindVertexBuffers)();

    RHIBuffer** ppVertexBuffers = AllocateCmdData<RHIBuffer*>(vertexBuffers.size());

    if (ppVertexBuffers != nullptr)
    {
        std::ranges::copy(vertexBuffers, ppVertexBuffers);
    }

    uint64_t* pOffsets = AllocateCmdData<uint64_t>(offsets.size());

    if (pOffsets != nullptr)
    {
        std::ranges::copy(offsets, pOffsets);
    }

    pCmd->vertexBuffers = MakeVecView(ppVertexBuffers, vertexBuffers.size());
    pCmd->offsets       = MakeVecView(pOffsets, offsets.size());
}

void RHICommandList::BindVertexBuffer(RHIBuffer* pBuffer, uint64_t offset)
{
    RetainResource(pBuffer);
    ALLOC_CMD(RHICommandBindVertexBuffer)(pBuffer, offset);
}

void RHICommandList::Draw(uint32_t vertexCount,
                          uint32_t instanceCount,
                          uint32_t firstVertex,
                          uint32_t firstInstance)
{
    ALLOC_CMD(RHICommandDraw)(vertexCount, instanceCount, firstVertex, firstInstance, GetContext());
}

void RHICommandList::DrawIndexed(const RHICommandDrawIndexed::Param& param)
{
    RetainResource(param.pIndexBuffer);
    ALLOC_CMD(RHICommandDrawIndexed)(param, GetContext());
}

void RHICommandList::DrawIndexedIndirect(const RHICommandDrawIndexedIndirect::Param& param)
{
    RetainResource(param.pIndexBuffer);
    RetainResource(param.pIndirectBuffer);
    ALLOC_CMD(RHICommandDrawIndexedIndirect)(param, GetContext());
}

void RHICommandList::Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{
    ALLOC_CMD(RHICommandDispatch)(groupCountX, groupCountY, groupCountZ, GetContext());
}

void RHICommandList::DispatchIndirect(RHIBuffer* pIndirectBuffer, uint32_t offset)
{
    RetainResource(pIndirectBuffer);
    ALLOC_CMD(RHICommandDispatchIndirect)(pIndirectBuffer, offset, GetContext());
}

void RHICommandList::SetPushConstants(RHIPipeline* pPipeline,
                                      const uint8_t* pData,
                                      uint32_t sizeBytes,
                                      uint32_t offset)
{
    RetainResource(pPipeline);
    RHICommandSetPushConstants* pCmd = ALLOC_CMD(RHICommandSetPushConstants)(pPipeline);

    uint8_t* pAllocatedData = AllocateCmdData<uint8_t>(sizeBytes);

    if (pData != nullptr)
    {
        std::memcpy(pAllocatedData, pData, sizeBytes);
    }

    pCmd->data   = MakeVecView(static_cast<const uint8_t*>(pAllocatedData), sizeBytes);
    pCmd->offset = offset;
}

void RHICommandList::AddTransitions(BitField<RHIPipelineStageFlagBits> srcStages,
                                    BitField<RHIPipelineStageFlagBits> dstStages,
                                    VectorView<RHIMemoryTransition> memoryTransitions,
                                    VectorView<RHIBufferTransition> bufferTransitions,
                                    VectorView<RHITextureTransition> textureTransitions)
{
    for (const RHIBufferTransition& transition : bufferTransitions)
    {
        RetainResource(transition.pBuffer);
    }
    for (const RHITextureTransition& transition : textureTransitions)
    {
        RetainResource(transition.pTexture);
    }
    RHICommandAddTransitions* pCmd = ALLOC_CMD(RHICommandAddTransitions)(srcStages, dstStages);

    RHIMemoryTransition* pMemoryTransitions =
        AllocateCmdData<RHIMemoryTransition>(memoryTransitions.size());

    if (pMemoryTransitions != nullptr)
    {
        std::ranges::copy(memoryTransitions, pMemoryTransitions);
    }

    RHIBufferTransition* pBufferTransitions =
        AllocateCmdData<RHIBufferTransition>(bufferTransitions.size());

    if (pBufferTransitions != nullptr)
    {
        std::ranges::copy(bufferTransitions, pBufferTransitions);
    }

    RHITextureTransition* pTextureTransitions =
        AllocateCmdData<RHITextureTransition>(textureTransitions.size());

    if (pTextureTransitions != nullptr)
    {
        std::ranges::copy(textureTransitions, pTextureTransitions);
    }

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
    RetainResource(pTexture);
    ALLOC_CMD(RHICommandAddTextureTransition)(pTexture, newLayout);
}
} // namespace zen
