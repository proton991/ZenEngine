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

void IRHICommandContext::OnFinalRelease()
{
    const bool accepted = GetRHIThread().DispatchCleanup([this] { ZEN_DELETE(this); });
    VERIFY_EXPR_MSG(accepted, "Context released after RHI cleanup admission closed");
}

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
    std::swap(result->m_submissionDependencies, m_submissionDependencies);

    std::swap(result->m_bindlessEpochs, m_bindlessEpochs);
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
    m_contextOwner.Reset();
    m_pGraphicsContext = nullptr;
    m_pComputeContext  = nullptr;
}

uint64_t RHICommandListBase::CaptureBindlessEpoch()
{
    IRHICommandContext* context = GetContext();
    uint64_t epoch              = context != nullptr ? context->RHIGetCurrentBindlessEpoch() : 0;

    if (epoch != 0 && (m_bindlessEpochs.empty() || m_bindlessEpochs.back() != epoch))
    {
        epoch = context->RHICaptureBindlessEpoch();

        if (epoch != 0 && (m_bindlessEpochs.empty() || m_bindlessEpochs.back() != epoch))
        {
            m_bindlessEpochs.push_back(epoch);
        }
        else if (epoch != 0)
        {
            context->RHIReleaseBindlessEpoch(epoch);
        }
    }

    return epoch;
}

void RHICommandListBase::ReleaseBindlessEpochs(size_t count)
{
    VERIFY_EXPR(count <= m_bindlessEpochs.size());

    for (size_t index = count; index < m_bindlessEpochs.size(); ++index)
    {
        GetContext()->RHIReleaseBindlessEpoch(m_bindlessEpochs[index]);
    }

    m_bindlessEpochs.resize(count);
}

void RHICommandListBase::Execute()
{
    GetRHIThread().CheckOwnership();
    RHICommandBase* pCmd = m_pCmdHead;

    const IRHICommandContext* context = GetContext();

    while (pCmd)
    {
        RHICommandBase* pNext = pCmd->pNextCmd; // Save next before freeing

        if (context == nullptr || !context->HasRecordingError())
        {
            static_cast<RHICommand*>(pCmd)->Execute(*this);
        }

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
    ReleaseBindlessEpochs(0);

    m_resources.Reset();
    m_submissionDependencies.clear();
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
    ReleaseBindlessEpochs(checkpoint.captureCount);

    m_resources.Rollback(checkpoint.resourceCount);
    m_submissionDependencies.resize(checkpoint.dependencyCount);
}

RHICommandList* RHICommandList::Create(IRHICommandContext* pContext)
{
    RHICommandList* pCmdList = nullptr;

    if (pContext != nullptr)
    {
        pCmdList                          = ZEN_NEW() RHICommandList();
        pCmdList->m_contextOwner          = RefCountPtr<IRHICommandContext>(pContext);
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
    }

    return pCmdList;
}

void RHICommandList::ClearBuffer(RHIBuffer* pBuffer, uint64_t offset, uint64_t size)
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

void RHICommandList::BeginDebugLabel(NameID name)
{
    ALLOC_CMD(RHICommandDebugLabel)(name, true);
}

void RHICommandList::EndDebugLabel()
{
    ALLOC_CMD(RHICommandDebugLabel)(NameID(), false);
}

void RHICommandList::BeginGPUTiming(const RHIGPUTimingPtr& result)
{
    ALLOC_CMD(RHICommandGPUTiming)(result, true);
}

void RHICommandList::EndGPUTiming(const RHIGPUTimingPtr& result)
{
    ALLOC_CMD(RHICommandGPUTiming)(result, false);
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
    ALLOC_CMD(RHICommandSetShaderParameters)(
        *this, parameters.GetView(),
        parameters.GetBindlessParams().empty() ? 0 : CaptureBindlessEpoch());
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
    ALLOC_CMD(RHICommandDraw)(vertexCount, instanceCount, firstVertex, firstInstance,
                              CaptureBindlessEpoch());
}

void RHICommandList::DrawIndexed(const RHICommandDrawIndexed::Param& param)
{
    RetainResource(param.pIndexBuffer);
    ALLOC_CMD(RHICommandDrawIndexed)(param, CaptureBindlessEpoch());
}

void RHICommandList::DrawIndexedIndirect(const RHICommandDrawIndexedIndirect::Param& param)
{
    RetainResource(param.pIndexBuffer);
    RetainResource(param.pIndirectBuffer);
    ALLOC_CMD(RHICommandDrawIndexedIndirect)(param, CaptureBindlessEpoch());
}

void RHICommandList::Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{
    ALLOC_CMD(RHICommandDispatch)(groupCountX, groupCountY, groupCountZ, CaptureBindlessEpoch());
}

void RHICommandList::DispatchIndirect(RHIBuffer* pIndirectBuffer, uint64_t offset)
{
    RetainResource(pIndirectBuffer);
    ALLOC_CMD(RHICommandDispatchIndirect)(pIndirectBuffer, offset, CaptureBindlessEpoch());
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
}

} // namespace zen
