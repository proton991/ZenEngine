#include "Utils/MetricsLogger.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/TextureManager.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Utils/Helpers.h"
#include "Utils/Errors.h"
#include "Graphics/RHI/RHIOptions.h"

#include <algorithm>
#include <bit>
#include <limits>

zen::rc::RenderFrameState GRenderFrameState;

namespace zen::rc
{
namespace
{
RHITextureCreateInfo MakeTextureInfo(const TextureFormat& format,
                                     TextureUsageHint hint,
                                     NameID name)
{
    RHITextureCreateInfo info{};
    info.type          = static_cast<RHITextureType>(format.dimension);
    info.format        = format.format;
    info.samples       = format.sampleCount;
    info.width         = format.width;
    info.height        = std::max(1u, format.height);
    info.depth         = std::max(1u, format.depth);
    info.arrayLayers   = format.arrayLayers;
    info.mipmaps       = format.mipmaps;
    info.mutableFormat = format.mutableFormat;
    info.tag           = name;

    if (hint.copyUsage)
    {
        info.usageFlags.SetFlags(RHITextureUsageFlagBits::eTransferSrc,
                                 RHITextureUsageFlagBits::eTransferDst);
    }

    return info;
}

size_t AlignBufferSize(size_t size, size_t alignment)
{
    alignment = std::max(size_t(1), alignment);
    VERIFY_EXPR_MSG(size <= std::numeric_limits<uint32_t>::max() - (alignment - 1),
                    "Buffer size overflow");

    return (size + alignment - 1) / alignment * alignment;
}
} // namespace

RenderDevice::RenderDevice(RHIAPIType APIType, uint32_t numFrames, RHIExecutionMode executionMode) :
    m_APIType(APIType),
    m_numFrames(std::clamp(numFrames, 1u, RenderFrameState::kMaxFramesInFlight)),
    m_executionMode(executionMode),
    m_frames(m_numFrames),
    m_rdgExecutor(this),
    m_rdgPassCompiler(this, nullptr),
    m_pipelineCache(kPipelineCacheCapacity, [this](const PipelineKey&, RHIPipeline*& pipeline) {
        ++m_pipelineMetrics.evictions;
        DeferDestroyPipeline(pipeline);
    })
{
    GRenderFrameState.Init(m_numFrames);

    if (GDynamicRHI == nullptr)
    {
        DynamicRHI::Create(m_APIType);
    }

    VERIFY_EXPR_MSG(GDynamicRHI != nullptr, "Failed to create the RHI");
    m_pRHIExecutor = ZEN_NEW() RHICommandListExecutor(GDynamicRHI, executionMode);
    GDynamicRHI    = m_pRHIExecutor;
    GetRHIThread().Invoke(&RHIFrameState::Init, &GRHIFrameState, m_numFrames);
    m_pRHIDebug = RHIDebug::Create();
}

void RenderDevice::Init(RHIViewport* viewport)
{
    VERIFY_EXPR_MSG(m_pUploadQueue == nullptr, "RenderDevice is already initialized");
    m_pImmediateTransferCmdList = RHICommandList::Create(GDynamicRHI->GetTransferCommandContext());
    m_pUploadQueue              = ZEN_NEW() StagingUploadQueue(this, &m_stagingBufferManager);
    m_pTextureManager           = ZEN_NEW() TextureManager(this, m_pUploadQueue);
    m_frameRDG                  = MakeUnique<RenderGraph>("frame_rdg");
    m_pMainViewport             = viewport;
    BeginFrame();

    if (viewport != nullptr)
    {
        m_pRendererServer = ZEN_NEW() RendererServer(this, viewport);
        m_pRendererServer->Init();
    }
}

void RenderDevice::Destroy()
{
    if (GDynamicRHI != nullptr)
    {
        if (m_pUploadQueue != nullptr)
        {
            m_pUploadQueue->Flush();
        }

        WaitForPreviousFrames();
        m_frameRDG.Reset();
        m_rdgPassCompiler.SetRenderGraph(nullptr);

        if (m_pRendererServer != nullptr)
        {
            m_pRendererServer->Destroy();
            ZEN_DELETE(m_pRendererServer);
            m_pRendererServer = nullptr;
        }

        if (m_pTextureManager != nullptr)
        {
            m_pTextureManager->Destroy();
            ZEN_DELETE(m_pTextureManager);
            m_pTextureManager = nullptr;
        }

        if (m_pUploadQueue != nullptr)
        {
            m_pUploadQueue->Destroy();
            ZEN_DELETE(m_pUploadQueue);
            m_pUploadQueue = nullptr;
        }

        m_stagingBufferManager.Destroy();

        for (PipelineCache::value_type& pipelineEntry : m_pipelineCache)
        {
            GDynamicRHI->DestroyPipeline(pipelineEntry.second);
        }

        m_pipelineCache.clear();

        for (HashMap<size_t, RHISampler*>::value_type& samplerEntry : m_samplerCache)
        {
            GDynamicRHI->DestroySampler(samplerEntry.second);
        }

        m_samplerCache.clear();

        for (RHIBuffer* buffer : m_buffers)
        {
            GDynamicRHI->DestroyBuffer(buffer);
        }

        m_buffers.clear();
        m_deletionQueue.Flush();

        for (uint32_t i = 0; i < m_numFrames; ++i)
        {
            ProcessPendingFreeResources(static_cast<RenderFrameSlot>(i), true);
        }

        while (!m_viewports.empty())
        {
            DestroyViewport(m_viewports.back());
        }

        m_pMainViewport = nullptr;

        for (RHIRenderingLayout* layout : m_renderingLayoutPool)
        {
            ZEN_DELETE(layout);
        }

        m_renderingLayoutPool.clear();

        for (GraphicsPass* pass : m_gfxPassPool)
        {
            ZEN_DELETE(pass);
        }

        m_gfxPassPool.clear();
        m_graphicsCmdListPool.Destroy();

        if (m_pImmediateTransferCmdList != nullptr)
        {
            m_pImmediateTransferCmdList->Reset();
            ZEN_DELETE(m_pImmediateTransferCmdList);
            m_pImmediateTransferCmdList = nullptr;
        }

        ZEN_DELETE(m_pRHIDebug);
        m_pRHIDebug = nullptr;
        GDynamicRHI->Destroy();
        CollectDestroyedResourceHistory();
        ZEN_DELETE(GDynamicRHI);
        GDynamicRHI       = nullptr;
        m_pRHIExecutor    = nullptr;
        m_frameActive     = false;
        m_frameWaitFailed = false;
    }
}

bool RenderDevice::ExecuteRenderGraph(RHIViewport* viewport)
{
    bool result{};

    if (m_executionMode == RHIExecutionMode::eThreaded)
    {
        result = QueueRenderGraph(viewport);
    }
    else if (!(viewport == nullptr || m_frameRDG.Get() == nullptr ||
               m_pImmediateTransferCmdList == nullptr || m_frameWaitFailed))
    {
        RenderGraph& graph = *m_frameRDG;

        if (m_submissionBlocked)
        {
            result = graph.Fail(
                RDGErrorCode::eSubmission,
                "GPU execution is blocked; recreate the device after submission failure");
        }
        else
        {
            RDGExecutor::ExecutionPlan plan;

            if (!m_rdgExecutor.PrepareExecution(&graph, plan))
            {
                result = false;
            }
            else if (!m_pUploadQueue->Flush())
            {
                result = graph.Fail(RDGErrorCode::eSubmission,
                                    "Pending uploads failed before frame execution");
            }
            else if (!m_rdgExecutor.RefreshExecution(plan))
            {
                result = false;
            }
            else
            {
                HeapVector<RHICommandList*> lists;
                AcquireGraphicsCmdLists(2, lists);
                const bool executed = m_rdgExecutor.ExecutePrepared(plan, lists[0], [this, &lists] {
                    return SubmitCommandLists(MakeVecView(lists.data(), 1));
                });
                m_graphicsCmdListPool.Release(lists[0]);

                if (!executed)
                {
                    m_graphicsCmdListPool.Release(lists[1]);
                    result = false;
                }
                else
                {
                    // Presentation records backend copy work into the dedicated graphics context.
                    GetRHIThread().Invoke(&RHIViewport::PrepareForPresent, viewport, lists[1]);
                    const RHISubmissionResult presentation =
                        SubmitCommandLists(MakeVecView(lists.data() + 1, 1));

                    if (presentation != RHISubmissionResult::eSuccess)
                    {
                        m_graphicsCmdListPool.Release(lists[1]);

                        // The render graph was already accepted. Keep its committed state and retirement gates.
                        if (presentation == RHISubmissionResult::eFatal)
                        {
                            RHITexture* backBuffer = viewport->GetColorBackBuffer();
                            if (backBuffer != nullptr)
                            {
                                m_rdgExecutor.GetResourceStateTracker().RemoveResourceState(
                                    backBuffer->GetStableId(), true);
                            }
                        }

                        result = graph.Fail(RDGErrorCode::eSubmission,
                                            "Presentation copy submission failed");
                    }
                    else
                    {
                        const bool presented =
                            GetRHIThread().Invoke(&RHIViewport::Present, viewport);
                        m_graphicsCmdListPool.Release(lists[1]);
                        m_rdgExecutor.GetResourceStateTracker().UpdateTextureState(
                            viewport->GetColorBackBuffer(), RHIAccessMode::eReadWrite,
                            RHITextureUsage::eColorAttachment,
                            BitField<RHIPipelineStageFlagBits>(
                                RHIPipelineStageFlagBits::eColorAttachmentOutput));
                        EndFrame();
                        result = presented && !m_frameActive;
                    }
                }
            }
        }
    }

    return result;
}

bool RenderDevice::QueueRenderGraph(RHIViewport* viewport)
{
    bool result = false;
    PollFrameSubmissions(false);
    if (viewport != nullptr && m_frameRDG.Get() != nullptr && m_frameActive && !m_frameWaitFailed &&
        !AreSubmissionsBlocked() && m_pRecreateViewport == nullptr)
    {
        RenderGraph& graph = *m_frameRDG;
        RDGExecutor::ExecutionPlan plan;
        if (m_rdgExecutor.PrepareExecution(&graph, plan) &&
            (m_pUploadQueue == nullptr || m_pUploadQueue->Flush()) &&
            m_rdgExecutor.RefreshExecution(plan))
        {
            RHICommandList* commands = m_graphicsCmdListPool.Acquire();
            PendingFrame pending;
            pending.frame    = &m_frames[ToIndex(GRenderFrameState.GetFrameSlot())];
            pending.viewport = viewport;
            result           = m_rdgExecutor.ExecutePrepared(
                plan, commands,
                std::bind_front(&RenderDevice::QueueRecordedFrame, this, std::ref(graph),
                                std::ref(*commands), viewport, std::ref(pending)),
                true);
            m_graphicsCmdListPool.Release(commands);
            if (result)
            {
                // Scheduled state belongs to RenderCore. Confirmation comes back through
                // the ticket; the worker never touches this graph or its pass callbacks.
                m_rdgExecutor.GetResourceStateTracker().UpdateTextureState(
                    viewport->GetColorBackBuffer(), RHIAccessMode::eReadWrite,
                    RHITextureUsage::eColorAttachment,
                    BitField<RHIPipelineStageFlagBits>(
                        RHIPipelineStageFlagBits::eColorAttachmentOutput));
                pending.scheduledState    = m_rdgExecutor.GetResourceStateTracker();
                pending.frame->submission = pending.ticket;
                m_pendingFrames.push_back(std::move(pending));
                // The queued frame batch owns native EndFrame, including on the slow path.
                m_frameActive = false;
            }
        }
    }
    CollectDestroyedResourceHistory();
    return result;
}

RHISubmissionResult RenderDevice::QueueRecordedFrame(RenderGraph& graph,
                                                     RHICommandList& commands,
                                                     RHIViewport* viewport,
                                                     PendingFrame& pending)
{
    // Allow preparation of the next frame while one frame is on the RHI thread.
    // Apply backpressure only at the next handoff, not after every dispatch.
    PollFrameSubmissions(true);
    RHISubmissionResult result = RHISubmissionResult::eRejected;
    if (AreSubmissionsBlocked())
    {
        result = RHISubmissionResult::eFatal;
    }
    else if (m_pRecreateViewport == nullptr)
    {
        graph.m_resourceManager.StageExtractions(pending.extractions);
        pending.ticket = m_pRHIExecutor->SubmitFrame(commands, viewport);
        result =
            pending.ticket.IsValid() ? RHISubmissionResult::eSuccess : RHISubmissionResult::eFatal;
    }
    return result;
}

void RenderDevice::CompleteFrame(PendingFrame& pending, const RHIBatchResult& result)
{
    RenderFrame& frame   = *pending.frame;
    frame.graphicsSerial = std::max(frame.graphicsSerial, result.serials[0]);
    frame.transferSerial = std::max(frame.transferSerial, result.serials[2]);
    frame.submission     = {};
    if (result.submission == RHISubmissionResult::eSuccess)
    {
        m_confirmedResourceState = pending.scheduledState;
        for (const RDGDeferredExtraction& extraction : pending.extractions)
        {
            if (extraction.state->resource == nullptr)
            {
                extraction.resource->AddReference();
                extraction.state->resource = extraction.resource.get();
                extraction.state->device   = this;
            }
        }
        if (result.needsRecreation)
        {
            m_pRecreateViewport = pending.viewport;
        }
    }
    else
    {
        m_submissionBlocked                     = true;
        m_rdgExecutor.GetResourceStateTracker() = ResourceStateTracker();
        m_confirmedResourceState                = ResourceStateTracker();
        LOGE("RHI submission failed: {}; recreate the device before continuing", result.error);
    }
}

bool RenderDevice::PollFrameSubmissions(bool wait)
{
    while (!m_pendingFrames.empty() && (wait || m_pendingFrames[0].ticket.IsReady()))
    {
        PendingFrame& pending       = m_pendingFrames[0];
        const RHIBatchResult result = pending.ticket.Wait();
        CompleteFrame(pending, result);
        m_pendingFrames.pop_front();
    }
    CollectDestroyedResourceHistory();
    return !AreSubmissionsBlocked();
}

void RenderDevice::CollectDestroyedResourceHistory()
{
    // A submission callback can poll while ExecutePrepared still owns a private
    // tracker/metrics copy. Defer draining until that copy commits or rolls back.
    if (m_pRHIExecutor != nullptr && !m_rdgExecutor.m_executing)
    {
        m_pRHIExecutor->DrainDestroyedResourceIds(m_destroyedResourceIds);
        for (uint64_t resourceId : m_destroyedResourceIds)
        {
            m_rdgExecutor.GetResourceStateTracker().RemoveResourceState(resourceId);
            m_confirmedResourceState.RemoveResourceState(resourceId);
            for (PendingFrame& pending : m_pendingFrames)
            {
                pending.scheduledState.RemoveResourceState(resourceId);
            }
        }
        m_destroyedResourceIds.clear();
    }
}

void RenderDevice::FlushRHIThread()
{
    if (m_pRHIExecutor != nullptr)
    {
        m_pRHIExecutor->FlushRHIThread();
        PollFrameSubmissions(true);
    }
}

RHIThreadMetrics RenderDevice::GetRHIThreadMetrics() const
{
    return m_pRHIExecutor != nullptr ? m_pRHIExecutor->GetThreadMetrics() : RHIThreadMetrics{};
}

void RenderDevice::ProcessDeferredViewportResize()
{
    if (m_pRecreateViewport != nullptr && !AreSubmissionsBlocked())
    {
        RHIViewport* viewport = m_pRecreateViewport;
        m_pRecreateViewport   = nullptr;
        ResizeViewport(viewport, viewport->GetWidth(), viewport->GetHeight());
    }
}

bool RenderDevice::ExecuteRenderGraph(RenderGraph& graph)
{
    bool result{};
    PollFrameSubmissions(true);

    if (m_submissionBlocked)
    {
        result =
            graph.Fail(RDGErrorCode::eSubmission,
                       "GPU execution is blocked; recreate the device after submission failure");
    }
    else if (m_frameWaitFailed)
    {
        LOGE("RenderDevice: graph execution blocked until the frame slot completes");

        result = false;
    }
    else if (m_pImmediateTransferCmdList == nullptr)
    {
        result = graph.Fail(RDGErrorCode::eLifecycle,
                            "RenderDevice must be initialized before graph execution");
    }
    else
    {
        RDGExecutor::ExecutionPlan plan;

        if (!m_rdgExecutor.PrepareExecution(&graph, plan))
        {
            result = false;
        }
        else if (m_pUploadQueue != nullptr && !m_resolvingStagingFlush && !m_pUploadQueue->Flush())
        {
            result = graph.Fail(RDGErrorCode::eSubmission,
                                "Pending uploads failed before graph execution");
        }
        else if (!m_rdgExecutor.RefreshExecution(plan))
        {
            result = false;
        }
        else
        {
            const bool transfer = plan.transfer;
            RHICommandList* list =
                transfer ? m_pImmediateTransferCmdList : m_graphicsCmdListPool.Acquire();

            if (list == nullptr)
            {
                result = graph.Fail(RDGErrorCode::eLifecycle,
                                    "RenderDevice must be initialized before graph execution");
            }
            else
            {
                const bool executed =
                    m_rdgExecutor.ExecutePrepared(plan, list, [this, transfer, &list] {
                        return transfer ? SubmitImmediateTransferCmdList() :
                                          SubmitCommandLists(MakeVecView(&list, 1));
                    });

                // Keep CPU command storage alive through rollback, then discard it on both paths.
                if (transfer)
                {
                    list->Reset();
                }
                else
                {
                    m_graphicsCmdListPool.Release(list);
                }

                result = executed;
            }
        }
    }

    CollectDestroyedResourceHistory();
    return result;
}

RHISubmissionResult RenderDevice::SubmitImmediateTransferCmdList()
{
    RHISubmissionResult returnValue{};

    const RHISubmissionResult result =
        SubmitCommandLists(MakeVecView(&m_pImmediateTransferCmdList, 1));

    if (result != RHISubmissionResult::eSuccess)
    {
        returnValue = result;
    }
    else
    {
        // There is no GPU cross-queue wait API. A failed CPU wait cannot establish visibility.
        if (!GDynamicRHI->IsTransferQueueSharedWithGraphics() &&
            !GDynamicRHI->WaitForSubmission(
                RHICommandContextType::eTransfer,
                GDynamicRHI->GetLastSubmittedSerial(RHICommandContextType::eTransfer)))
        {
            m_submissionBlocked = true;
            returnValue         = RHISubmissionResult::eFatal;
        }
        else
        {
            returnValue = RHISubmissionResult::eSuccess;
        }
    }

    return returnValue;
}

void RenderDevice::InvalidateRDGPassCompilerForResize()
{
    ++m_pipelineMetrics.invalidations;
    m_pipelineMetrics.invalidatedEntries += m_pipelineCache.size();

    if (m_frameRDG.Get() != nullptr)
    {
        m_frameRDG->ResetBuildState();
        m_frameRDG->TrimCompiledPassStorage();
        m_frameRDG->GetResourceManager()->TrimPool(true);
    }

    m_rdgPassCompiler.SetRenderGraph(nullptr);

    for (PipelineCache::value_type& pipelineEntry : m_pipelineCache)
    {
        DeferDestroyPipeline(pipelineEntry.second);
    }

    m_pipelineCache.clear();
}

void RenderDevice::InvalidateExternalTextureState(RHITexture* texture)
{
    if (texture != nullptr)
    {
        m_rdgExecutor.GetResourceStateTracker().RemoveResourceState(texture->GetStableId(), true);
        m_confirmedResourceState.RemoveResourceState(texture->GetStableId(), true);
    }
}

void RenderDevice::InvalidateExternalBufferState(RHIBuffer* buffer)
{
    if (buffer != nullptr)
    {
        m_rdgExecutor.GetResourceStateTracker().RemoveResourceState(buffer->GetStableId(), true);
        m_confirmedResourceState.RemoveResourceState(buffer->GetStableId(), true);
    }
}

bool RenderDevice::ResolveStagingFlushAction(StagingFlushAction action,
                                             StagingBufferManager* manager)
{
    bool returnValue{};

    if (action == StagingFlushAction::eNone)
    {
        returnValue = true;
    }
    else if (m_resolvingStagingFlush)
    {
        returnValue = false;
    }
    else
    {
        m_resolvingStagingFlush = true;

        if (m_pUploadQueue != nullptr)
        {
            m_pUploadQueue->Flush();
        }

        const RHISubmissionResult result = GDynamicRHI->FlushAllGPUCommands();
        StampOutgoingFrameSerials();
        m_submissionBlocked |= result == RHISubmissionResult::eFatal;
        const bool completed = !m_submissionBlocked && result == RHISubmissionResult::eSuccess &&
            (manager != nullptr ? manager : &m_stagingBufferManager)->WaitForSubmittedAllocations();
        m_resolvingStagingFlush = false;
        returnValue             = completed;
    }

    return returnValue;
}

RHIRenderingLayout* RenderDevice::AcquireRenderingLayout()
{
    RHIRenderingLayout* layout = m_renderingLayoutPool.empty() ? ZEN_NEW() RHIRenderingLayout() :
                                                                 m_renderingLayoutPool.back();

    if (!m_renderingLayoutPool.empty())
    {
        m_renderingLayoutPool.pop_back();
    }

    *layout = RHIRenderingLayout{};

    return layout;
}

void RenderDevice::ReleaseRenderingLayout(RHIRenderingLayout* layout)
{
    if (layout != nullptr)
    {
        *layout = RHIRenderingLayout{};
        m_renderingLayoutPool.push_back(layout);
    }
}

void RenderDevice::DestroyRenderingLayout(RHIRenderingLayout* layout)
{
    RHIRenderingLayout** it =
        std::find(m_renderingLayoutPool.begin(), m_renderingLayoutPool.end(), layout);

    if (it != m_renderingLayoutPool.end())
    {
        m_renderingLayoutPool.erase(it);
    }

    ZEN_DELETE(layout);
}

RHITexture* RenderDevice::CreateTextureColorRT(const TextureFormat& format,
                                               TextureUsageHint hint,
                                               NameID name)
{
    RHITextureCreateInfo info = MakeTextureInfo(format, hint, name);
    info.usageFlags.SetFlags(RHITextureUsageFlagBits::eColorAttachment,
                             RHITextureUsageFlagBits::eSampled);

    return GDynamicRHI->CreateTexture(info);
}

RHITexture* RenderDevice::CreateTextureDepthStencilRT(const TextureFormat& format,
                                                      TextureUsageHint hint,
                                                      NameID name)
{
    RHITextureCreateInfo info = MakeTextureInfo(format, hint, name);
    info.usageFlags.SetFlags(RHITextureUsageFlagBits::eDepthStencilAttachment,
                             RHITextureUsageFlagBits::eSampled);

    return GDynamicRHI->CreateTexture(info);
}

RHITexture* RenderDevice::CreateTextureStorage(const TextureFormat& format,
                                               TextureUsageHint hint,
                                               NameID name)
{
    RHITextureCreateInfo info = MakeTextureInfo(format, hint, name);
    info.usageFlags.SetFlags(RHITextureUsageFlagBits::eStorage, RHITextureUsageFlagBits::eSampled);

    return GDynamicRHI->CreateTexture(info);
}

RHITexture* RenderDevice::CreateTextureSampled(const TextureFormat& format,
                                               TextureUsageHint hint,
                                               NameID name)
{
    RHITextureCreateInfo info = MakeTextureInfo(format, hint, name);
    info.usageFlags.SetFlag(RHITextureUsageFlagBits::eSampled);

    return GDynamicRHI->CreateTexture(info);
}

RHITexture* RenderDevice::CreateTextureDummy(const TextureFormat& format,
                                             TextureUsageHint hint,
                                             NameID name)
{
    return CreateTextureSampled(format, hint, name);
}

RHITextureView* RenderDevice::CreateTextureView(RHITexture* texture,
                                                const TextureViewFormat& format,
                                                NameID name)
{
    VERIFY_EXPR_MSG(texture != nullptr, "Cannot create a view of a null texture");

    RHITextureViewCreateInfo info{};
    info.format = format.format == DataFormat::eUndefined ? texture->GetFormat() : format.format;
    info.type   = static_cast<RHITextureType>(format.dimension);
    info.arrayLayers  = format.arrayLayers;
    info.mipLevels    = format.mipmaps;
    info.baseMipLevel = format.baseMipLevel;
    info.tag          = name;
    VERIFY_EXPR_MSG(info.arrayLayers > 0 && info.arrayLayers <= texture->GetArrayLayers() &&
                        info.mipLevels > 0 &&
                        uint64_t(info.baseMipLevel) + info.mipLevels <= texture->GetNumMipmaps(),
                    "Invalid texture view range");

    return GDynamicRHI->CreateTextureView(texture, info);
}

void RenderDevice::DestroyTexture(RHITexture* texture)
{
    if (texture == nullptr)
    {
        return;
    }

    if (m_pUploadQueue != nullptr)
    {
        m_pUploadQueue->Flush();
    }

    m_frames[GetCurrentFrameSlot()].texturesPendingFree.push_back(texture);
    StampOutgoingFrameSerials();
}

void RenderDevice::UpdateBuffer(RHIBuffer* buffer,
                                uint32_t size,
                                const uint8_t* data,
                                uint32_t offset)
{
    UpdateBufferInternal(buffer, offset, size, data);
}

void RenderDevice::InitializeBufferData(RHIBuffer* buffer, uint32_t dataSize, const uint8_t* data)
{
    if (data == nullptr || dataSize == 0)
    {
        return;
    }

    const uint32_t size = buffer->GetRequiredSize();
    // Assemble only the final aligned word and padding, never read beyond caller data.
    // Both copies have aligned offsets/sizes; large payloads retain chunked staging.
    const uint32_t prefix = dataSize & ~uint32_t(3);

    if (prefix != 0)
    {
        UpdateBufferInternal(buffer, 0, prefix, data);
    }

    if (prefix < size)
    {
        HeapVector<uint8_t> tail(size - prefix);
        std::memcpy(tail.data(), data + prefix, dataSize - prefix);
        UpdateBufferInternal(buffer, prefix, static_cast<uint32_t>(tail.size()), tail.data());
    }
}

void RenderDevice::UpdateBufferInternal(RHIBuffer* buffer,
                                        uint32_t offset,
                                        uint32_t size,
                                        const uint8_t* data)
{
    if (data == nullptr || size == 0)
    {
        return;
    }

    VERIFY_EXPR_MSG(m_pUploadQueue != nullptr,
                    "RenderDevice must be initialized before buffer uploads");
    m_pUploadQueue->EnqueueBuffer(buffer, offset, size, data);
}

void RenderDevice::DestroyBuffer(RHIBuffer* buffer)
{
    if (buffer == nullptr)
    {
        return;
    }

    if (m_pUploadQueue != nullptr)
    {
        m_pUploadQueue->Flush();
    }

    HeapVector<RHIBuffer*>::iterator it = std::find(m_buffers.begin(), m_buffers.end(), buffer);

    if (it != m_buffers.end())
    {
        m_buffers.erase(it);
    }

    m_frames[GetCurrentFrameSlot()].buffersPendingFree.push_back(buffer);
    StampOutgoingFrameSerials();
}

void RenderDevice::DeferReleaseResource(RHIResource* resource)
{
    if (resource != nullptr)
    {
        m_frames[GetCurrentFrameSlot()].resourcesPendingRelease.push_back(resource);
        StampOutgoingFrameSerials();
    }
}

void RenderDevice::DeferDestroyPipeline(RHIPipeline* pipeline)
{
    if (pipeline != nullptr)
    {
        m_frames[GetCurrentFrameSlot()].pipelinesPendingFree.push_back(pipeline);
        StampOutgoingFrameSerials();
    }
}

RHIPipeline* RenderDevice::GetOrCreateGfxPipeline(const RHIGfxPipelineStates& states,
                                                  RHIShader* shader,
                                                  const RHIRenderingLayout* layout,
                                                  const HashMap<uint32_t, int>& constants)
{
    const RDGMetricsOptions& options = GetRDGMetrics().GetOptions();
    return GetOrCreateGfxPipeline(states, shader, layout, constants,
                                  options.logging.enabled && options.preparationTimings);
}

RHIPipeline* RenderDevice::GetOrCreateGfxPipeline(const RHIGfxPipelineStates& states,
                                                  RHIShader* shader,
                                                  const RHIRenderingLayout* layout,
                                                  const HashMap<uint32_t, int>& constants,
                                                  bool timed)
{
    RHIPipeline* pipeline = nullptr;

    if (shader != nullptr && layout != nullptr)
    {
        if (layout->numColorRenderTargets > MAX_NUM_COLOR_ATTACHMENTS ||
            states.colorBlendState.attachmentIdx > MAX_NUM_COLOR_ATTACHMENTS)
        {
            LOGE("Pipeline cache [{}]: attachment count exceeds the supported limit",
                 uint32_t(RDGErrorCode::eRange));
        }
        else
        {
            ++m_pipelineMetrics.requests;
            m_pipelineMetrics.timedRequests += timed;
            ScopedMetricsTimer keyTimer(timed, m_pipelineMetrics.keyCPUUs);
            PipelineKey key = MakePipelineKey(shader, &states, layout, constants,
                                              RHIOptions::GetInstance().UseDynamicRendering());
            keyTimer.Stop();
            ScopedMetricsTimer lookupTimer(timed, m_pipelineMetrics.lookupCPUUs);
            LRUCache<PipelineKey, RHIPipeline*, PipelineKeyHasher>::iterator it =
                m_pipelineCache.find(key);
            lookupTimer.Stop();

            if (it != m_pipelineCache.end())
            {
                ++m_pipelineMetrics.hits;
                pipeline = it->second;
            }
            else
            {
                ++m_pipelineMetrics.misses;
                ScopedMetricsTimer creationTimer(timed, m_pipelineMetrics.creationCPUUs);

                RHIGfxPipelineCreateInfo info{};
                info.pShader = shader;

                // Specialization belongs to this cache entry, not the shared shader.
                if (!constants.empty())
                {
                    RHIShaderCreateInfo shaderInfo = shader->GetCreateInfo();

                    for (const HashMap<uint32_t, int>::value_type& constant : constants)
                    {
                        shaderInfo.specializationConstants[constant.first] = constant.second;
                    }

                    info.pShader = GDynamicRHI->CreateShader(shaderInfo);
                }

                if (info.pShader != nullptr)
                {
                    struct ShaderGuard
                    {
                        RHIShader* specialized;

                        ~ShaderGuard()
                        {
                            if (specialized != nullptr)
                            {
                                GDynamicRHI->DestroyShader(specialized);
                            }
                        }
                    } guard{info.pShader != shader ? info.pShader : nullptr};

                    info.states           = states;
                    info.pRenderingLayout = layout;
                    info.subpassIdx       = 0;
                    pipeline              = GDynamicRHI->CreatePipeline(info);

                    if (pipeline != nullptr)
                    {
                        ++m_pipelineMetrics.creations;
                        creationTimer.Stop();
                        m_pipelineCache.try_emplace(std::move(key), pipeline);
                    }
                }

                if (pipeline == nullptr)
                {
                    ++m_pipelineMetrics.failures;
                }
            }
        }
    }

    return pipeline;
}

RHIPipeline* RenderDevice::GetOrCreateComputePipeline(RHIShader* shader)
{
    const RDGMetricsOptions& options = GetRDGMetrics().GetOptions();
    return GetOrCreateComputePipeline(shader,
                                      options.logging.enabled && options.preparationTimings);
}

RHIPipeline* RenderDevice::GetOrCreateComputePipeline(RHIShader* shader, bool timed)
{
    RHIPipeline* result{};

    if (shader != nullptr)
    {
        ++m_pipelineMetrics.requests;
        m_pipelineMetrics.timedRequests += timed;
        ScopedMetricsTimer keyTimer(timed, m_pipelineMetrics.keyCPUUs);
        PipelineKey key = MakePipelineKey(shader);
        keyTimer.Stop();
        ScopedMetricsTimer lookupTimer(timed, m_pipelineMetrics.lookupCPUUs);
        PipelineCache::iterator it = m_pipelineCache.find(key);
        lookupTimer.Stop();

        if (it != m_pipelineCache.end())
        {
            ++m_pipelineMetrics.hits;
            result = it->second;
        }
        else
        {
            ++m_pipelineMetrics.misses;
            ScopedMetricsTimer creationTimer(timed, m_pipelineMetrics.creationCPUUs);

            RHIComputePipelineCreateInfo info{};
            info.pShader          = shader;
            RHIPipeline* pipeline = GDynamicRHI->CreatePipeline(info);

            if (pipeline == nullptr)
            {
                ++m_pipelineMetrics.failures;
                result = nullptr;
            }
            else
            {
                ++m_pipelineMetrics.creations;
                creationTimer.Stop();
                m_pipelineCache.try_emplace(std::move(key), pipeline);
                result = pipeline;
            }
        }
    }

    return result;
}

RHIViewport* RenderDevice::CreateViewport(void* window, uint32_t width, uint32_t height, bool vsync)
{
    RHIViewport* viewport = GDynamicRHI->CreateViewport(window, width, height, vsync);

    if (viewport != nullptr)
    {
        m_viewports.push_back(viewport);
    }

    return viewport;
}

void RenderDevice::DestroyViewport(RHIViewport* viewport)
{
    if (viewport != nullptr)
    {
        WaitForPreviousFrames();
        InvalidateExternalTextureState(viewport->GetColorBackBuffer());
        InvalidateExternalTextureState(viewport->GetDepthStencilBackBuffer());
        HeapVector<RHIViewport*>::iterator it =
            std::find(m_viewports.begin(), m_viewports.end(), viewport);
        if (it != m_viewports.end())
        {
            m_viewports.erase(it);
        }
        if (m_pRecreateViewport == viewport)
        {
            m_pRecreateViewport = nullptr;
        }
        if (m_pMainViewport == viewport)
        {
            m_pMainViewport = nullptr;
        }
        GDynamicRHI->DestroyViewport(viewport);
    }
}

void RenderDevice::ResizeViewport(RHIViewport* viewport, uint32_t width, uint32_t height)
{
    if (viewport != nullptr && width != 0 && height != 0 &&
        (viewport->GetWidth() != width || viewport->GetHeight() != height ||
         GetRHIThread().Invoke(&RHIViewport::NeedsRecreation, viewport)))
    {
        if (m_pUploadQueue != nullptr)
        {
            m_pUploadQueue->Flush();
        }
        WaitForPreviousFrames();
        if (!AreSubmissionsBlocked())
        {
            InvalidateRDGPassCompilerForResize();
            InvalidateExternalTextureState(viewport->GetColorBackBuffer());
            InvalidateExternalTextureState(viewport->GetDepthStencilBackBuffer());
            GetRHIThread().Invoke(&RHIViewport::Resize, viewport, width, height);
            m_pRecreateViewport = nullptr;
        }
    }
}

void RenderDevice::ProcessViewportResize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
    {
        return;
    }

    ResizeViewport(m_pMainViewport, width, height);
}

void RenderDevice::WaitForPreviousFrames()
{
    PollFrameSubmissions(true);
    const RHISubmissionResult result = GDynamicRHI->FlushAllGPUCommands();
    StampOutgoingFrameSerials();
    m_submissionBlocked |= result == RHISubmissionResult::eFatal;
    GDynamicRHI->WaitDeviceIdle();
    CollectDestroyedResourceHistory();
}

void RenderDevice::StampOutgoingFrameSerials()
{
    RenderFrame& frame = m_frames[GetCurrentFrameSlot()];
    frame.graphicsSerial =
        std::max(frame.graphicsSerial,
                 GDynamicRHI->GetLastSubmittedSerial(RHICommandContextType::eGraphics));
    frame.transferSerial =
        std::max(frame.transferSerial,
                 GDynamicRHI->GetLastSubmittedSerial(RHICommandContextType::eTransfer));
}

void RenderDevice::CollectCompletedResources()
{
    if (m_pRHIExecutor != nullptr)
    {
        m_pRHIExecutor->PollGPUProgress();
    }
    PollFrameSubmissions(false);
    for (uint32_t slot = 0; slot < m_numFrames; ++slot)
    {
        ProcessPendingFreeResources(static_cast<RenderFrameSlot>(slot));
    }
    CollectDestroyedResourceHistory();
}

void RenderDevice::ProcessPendingFreeResources(RenderFrameSlot slot, bool ignoreCompletionGate)
{
    // A failed native call can leave work in use without an accepted serial. Retain owners
    // until Destroy has waited for the device, rather than trusting incomplete serial history.
    if (ignoreCompletionGate ||
        (!AreSubmissionsBlocked() && !m_frames[ToIndex(slot)].submission.IsValid()))
    {
        RenderFrame& frame = m_frames[ToIndex(slot)];

        if (!(!ignoreCompletionGate &&
              (GDynamicRHI->GetLastCompletedSerial(RHICommandContextType::eGraphics) <
                   frame.graphicsSerial ||
               GDynamicRHI->GetLastCompletedSerial(RHICommandContextType::eTransfer) <
                   frame.transferSerial)))
        {
            for (RHIBuffer* buffer : frame.buffersPendingFree)
            {
                if (buffer->GetRefCount() == 1)
                {
                    InvalidateExternalBufferState(buffer);
                }

                GDynamicRHI->DestroyBuffer(buffer);
            }

            for (RHITexture* texture : frame.texturesPendingFree)
            {
                if (texture->GetRefCount() == 1)
                {
                    InvalidateExternalTextureState(texture);
                }

                GDynamicRHI->DestroyTexture(texture);
            }

            for (RHIPipeline* pipeline : frame.pipelinesPendingFree)
            {
                GDynamicRHI->DestroyPipeline(pipeline);
            }

            for (RHIResource* resource : frame.resourcesPendingRelease)
            {
                // Owner retirement must not erase hazards while a replayable graph retains the object.
                if (resource->GetRefCount() == 1)
                {
                    if (resource->GetResourceType() == RHIResourceType::eBuffer)
                    {
                        InvalidateExternalBufferState(static_cast<RHIBuffer*>(resource));
                    }
                    else if (resource->GetResourceType() == RHIResourceType::eTexture)
                    {
                        InvalidateExternalTextureState(static_cast<RHITexture*>(resource));
                    }
                }

                resource->ReleaseReference();
            }

            frame.resourcesPendingRelease.clear();
            frame.buffersPendingFree.clear();
            frame.texturesPendingFree.clear();
            frame.pipelinesPendingFree.clear();
            frame.graphicsSerial = 0;
            frame.transferSerial = 0;
        }
    }
}

void RenderDevice::NextFrame()
{
    PollFrameSubmissions(false);
    ProcessDeferredViewportResize();
    if (m_frameWaitFailed)
    {
        BeginFrame(); // Retry the same slot; its resources are still in flight.
    }
    else
    {
        EndFrame();

        if (!(m_frameActive || m_submissionBlocked))
        {
            GRenderFrameState.Advance();
            BeginFrame();
        }
    }
}

void RenderDevice::BeginFrame()
{
    PollFrameSubmissions(false);
    if (!m_frameActive && !AreSubmissionsBlocked())
    {
        const RenderFrameSlot slot = GRenderFrameState.GetFrameSlot();
        const RenderFrame& frame   = m_frames[ToIndex(slot)];
        if (frame.submission.IsValid())
        {
            PollFrameSubmissions(true);
        }
        bool ready = !AreSubmissionsBlocked();
        for (const std::pair<RHICommandContextType, uint64_t>& completion :
             {std::pair{RHICommandContextType::eGraphics, frame.graphicsSerial},
              std::pair{RHICommandContextType::eTransfer, frame.transferSerial}})
        {
            if (ready &&
                GDynamicRHI->GetLastCompletedSerial(completion.first) < completion.second &&
                !GDynamicRHI->WaitForSubmission(completion.first, completion.second))
            {
                LOGE("RenderDevice: frame slot {} cannot reuse queue {} serial {}", ToIndex(slot),
                     uint32_t(completion.first), completion.second);
                m_frameWaitFailed = true;
                ready             = false;
            }
        }
        if (ready)
        {
            if (m_pUploadQueue != nullptr)
            {
                m_pUploadQueue->ReclaimResources();
            }
            ProcessPendingFreeResources(slot);
            m_stagingBufferManager.Reclaim();
            GDynamicRHI->BeginFrame();
            m_frameActive     = true;
            m_frameWaitFailed = false;
        }
    }
}

void RenderDevice::EndFrame()
{
    if ((!(!m_frameActive || m_submissionBlocked)) &&
        (!(m_pUploadQueue != nullptr && !m_pUploadQueue->Flush())))
    {
        const RHISubmissionResult result = GDynamicRHI->FlushAllGPUCommands();
        StampOutgoingFrameSerials();
        m_submissionBlocked |= result == RHISubmissionResult::eFatal;

        if (!(result != RHISubmissionResult::eSuccess))
        {
            GDynamicRHI->EndFrame();
            m_frameActive = false;
        }
    }
}

RHICommandList* GraphicsCommandListPoolPolicy::Create()
{
    return RHICommandList::Create(GDynamicRHI->GetCommandContext(RHICommandContextType::eGraphics));
}

void GraphicsCommandListPoolPolicy::Reset(RHICommandList* list)
{
    list->Reset();
}

void GraphicsCommandListPoolPolicy::Destroy(RHICommandList* list)
{
    list->Reset();
    ZEN_DELETE(list);
}

void RenderDevice::AcquireGraphicsCmdLists(size_t count, HeapVector<RHICommandList*>& lists)
{
    lists.reserve(lists.size() + count);

    for (size_t i = 0; i < count; ++i)
    {
        lists.push_back(m_graphicsCmdListPool.Acquire());
    }
}

RHISubmissionResult RenderDevice::SubmitCommandLists(VectorView<RHICommandList*> lists)
{
    RHISubmissionResult returnValue{};

    if (m_submissionBlocked)
    {
        returnValue = RHISubmissionResult::eFatal;
    }
    else if (lists.empty())
    {
        returnValue = RHISubmissionResult::eSuccess;
    }
    else
    {
        const RHISubmissionResult result = m_pRHIExecutor->SubmitBatch(lists);
        // Accepted prefixes must protect resources even if a later submission in this flush failed.
        StampOutgoingFrameSerials();
        m_submissionBlocked |= result == RHISubmissionResult::eFatal;
        returnValue = result;
    }

    return returnValue;
}

void RenderDevice::PipelineKey::AddWord(uint32_t value)
{
    words.push_back(value);
    util::HashCombine(hash, value);
}

template <typename T> void RenderDevice::PipelineKey::Add(T value)
{
    if constexpr (std::is_floating_point_v<T>)
    {
        AddWord(std::bit_cast<uint32_t>(value));
    }
    else if constexpr (sizeof(T) > sizeof(uint32_t))
    {
        AddWord(uint32_t(value));
        AddWord(uint32_t(uint64_t(value) >> 32));
    }
    else
    {
        AddWord(uint32_t(value));
    }
}

void RenderDevice::PipelineKey::AddStencil(const RHIStencilOpState& op)
{
    Add(op.fail);
    Add(op.pass);
    Add(op.depthFail);
    Add(op.compare);
    Add(op.compareMask);
    Add(op.writeMask);
    Add(op.reference);
}

void RenderDevice::PipelineKey::AddAttachment(const RHIRenderTarget& target, bool dynamicRendering)
{
    Add(target.format);
    Add(target.numSamples);
    if (dynamicRendering)
    {
        Add(int64_t(target.GetAspects()));
    }

    // Dynamic pipeline creation consumes attachment formats, not per-pass load/store ops.
    // Retain the conservative render-pass distinction for the legacy backend path.
    if (!dynamicRendering)
    {
        Add(target.loadOp);
        Add(target.storeOp);
    }
}

RenderDevice::PipelineKey RenderDevice::MakePipelineKey(RHIShader* shader,
                                                        const RHIGfxPipelineStates* graphics,
                                                        const RHIRenderingLayout* layout,
                                                        const HashMap<uint32_t, int>& constants,
                                                        bool dynamicRendering)
{
    PipelineKey key;

    key.Add(graphics ? RHIPipelineType::eGraphics : RHIPipelineType::eCompute);
    // ShaderProgram::Init replaces the shader object. Its stable ID distinguishes
    // pipeline entries without rescanning immutable shader bytecode.
    key.Add(shader->GetStableId());

    if (graphics != nullptr)
    {
        const RHIGfxPipelineStates& states = *graphics;
        key.Add(states.primitiveType);
        const RHIGfxPipelineRasterizationState& r = states.rasterizationState;
        key.Add(r.enableDepthClamp);
        key.Add(r.discardPrimitives);
        key.Add(r.wireframe);
        key.Add(r.cullMode);
        key.Add(r.frontFace);
        key.Add(r.enableDepthBias);
        key.Add(r.depthBiasConstantFactor);
        key.Add(r.depthBiasClamp);
        key.Add(r.depthBiasSlopeFactor);
        key.Add(r.lineWidth);
        const RHIGfxPipelineMultiSampleState& m = states.multiSampleState;
        key.Add(m.sampleCount);
        key.Add(m.enableSampleShading);
        key.Add(m.minSampleShading);
        key.Add(m.enableAlphaToCoverage);
        key.Add(m.enableAlphaToOne);
        key.Add(m.sampleMasks);
        const RHIGfxPipelineDepthStencilState& d = states.depthStencilState;
        key.Add(d.enableDepthTest);
        key.Add(d.enableDepthWrite);
        key.Add(d.depthCompareOp);
        key.Add(d.enableDepthBoundsTest);
        key.Add(d.enableStencilTest);
        key.Add(d.minDepthBounds);
        key.Add(d.maxDepthBounds);

        key.AddStencil(d.frontOp);
        key.AddStencil(d.backOp);
        const RHIGfxPipelineColorBlendState& c = states.colorBlendState;
        key.Add(c.enableLogicOp);
        key.Add(c.logicOp);
        key.Add(c.attachmentIdx);

        for (uint32_t i = 0; i < MAX_NUM_COLOR_ATTACHMENTS; ++i)
        {
            const bool enabled = c.attachmentsMask.Test(i);
            key.Add(enabled);

            if (!enabled)
            {
                continue;
            }

            const RHIGfxPipelineColorBlendState::Attachment& a = c.attachments[i];
            key.Add(a.enableBlend);
            key.Add(a.srcColorBlendFactor);
            key.Add(a.dstColorBlendFactor);
            key.Add(a.colorBlendOp);
            key.Add(a.srcAlphaBlendFactor);
            key.Add(a.dstAlphaBlendFactor);
            key.Add(a.alphaBlendOp);
            key.Add(int64_t(a.colorWriteMask));
        }

        key.Add(c.blendConstants.r);
        key.Add(c.blendConstants.g);
        key.Add(c.blendConstants.b);
        key.Add(c.blendConstants.a);

        for (uint32_t i = 0; i < ToUnderlying(RHIDynamicState::eMax); ++i)
        {
            key.Add(bool(states.dynamicStates.enabledStates.Test(i)));
        }

        key.Add(dynamicRendering);
        key.Add(layout->numColorRenderTargets);
        key.Add(layout->hasDepthStencilRT);

        for (uint32_t i = 0; i < layout->numColorRenderTargets; ++i)
        {
            key.AddAttachment(layout->colorRenderTargets[i], dynamicRendering);
        }

        if (layout->hasDepthStencilRT)
        {
            key.AddAttachment(layout->depthStencilRenderTarget, dynamicRendering);
        }

        SmallVector<std::pair<uint32_t, int>, 8> sorted;
        sorted.reserve(constants.size());

        for (const std::pair<const uint32_t, int>& item : constants)
        {
            sorted.emplace_back(item.first, item.second);
        }

        std::sort(sorted.begin(), sorted.end());
        key.Add(uint64_t(sorted.size()));

        for (const std::pair<uint32_t, int>& constant : sorted)
        {
            key.Add(constant.first);
            key.Add(constant.second);
        }
    }

    return key;
}

RHIBuffer* RenderDevice::CreateVertexBuffer(uint32_t dataSize, const uint8_t* pData)
{
    BitField<RHIBufferUsageFlagBits> usages;
    usages.SetFlag(RHIBufferUsageFlagBits::eVertexBuffer);
    usages.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);
    usages.SetFlag(RHIBufferUsageFlagBits::eStorageBuffer);

    RHIBufferCreateInfo createInfo{};
    createInfo.size         = dataSize;
    createInfo.usageFlags   = usages;
    createInfo.allocateType = RHIBufferAllocateType::eGPU;

    RHIBuffer* pVertexBuffer = GDynamicRHI->CreateBuffer(createInfo);
    UpdateBufferInternal(pVertexBuffer, 0, dataSize, pData);
    m_buffers.push_back(pVertexBuffer);

    return pVertexBuffer;
}

RHIBuffer* RenderDevice::CreateIndexBuffer(uint32_t dataSize, const uint8_t* pData)
{
    BitField<RHIBufferUsageFlagBits> usages;
    usages.SetFlag(RHIBufferUsageFlagBits::eIndexBuffer);
    usages.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);
    usages.SetFlag(RHIBufferUsageFlagBits::eStorageBuffer);

    RHIBufferCreateInfo createInfo{};
    createInfo.size         = dataSize;
    createInfo.usageFlags   = usages;
    createInfo.allocateType = RHIBufferAllocateType::eGPU;

    RHIBuffer* pIndexBuffer = GDynamicRHI->CreateBuffer(createInfo);
    UpdateBufferInternal(pIndexBuffer, 0, dataSize, pData);
    m_buffers.push_back(pIndexBuffer);

    return pIndexBuffer;
}

RHIBuffer* RenderDevice::CreateUniformBuffer(uint32_t dataSize,
                                             const uint8_t* pData,
                                             NameID bufferName)
{
    BitField<RHIBufferUsageFlagBits> usages;
    usages.SetFlag(RHIBufferUsageFlagBits::eUniformBuffer);
    usages.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);

    uint32_t paddedSize = PadUniformBufferSize(dataSize);

    RHIBufferCreateInfo createInfo{};
    createInfo.size         = paddedSize;
    createInfo.usageFlags   = usages;
    createInfo.allocateType = RHIBufferAllocateType::eGPU;
    createInfo.tag          = bufferName;

    RHIBuffer* pUniformBuffer = GDynamicRHI->CreateBuffer(createInfo);

    if (pData != nullptr)
    {
        InitializeBufferData(pUniformBuffer, dataSize, pData);
    }

    m_buffers.push_back(pUniformBuffer);

    return pUniformBuffer;
}

RHIBuffer* RenderDevice::CreateStorageBuffer(uint32_t dataSize,
                                             const uint8_t* pData,
                                             NameID bufferName)
{
    BitField<RHIBufferUsageFlagBits> usages;
    usages.SetFlag(RHIBufferUsageFlagBits::eStorageBuffer);
    usages.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);

    uint32_t paddedSize = PadStorageBufferSize(dataSize);

    RHIBufferCreateInfo createInfo{};
    createInfo.size         = paddedSize;
    createInfo.usageFlags   = usages;
    createInfo.allocateType = RHIBufferAllocateType::eGPU;
    createInfo.tag          = bufferName;

    RHIBuffer* pStorageBuffer = GDynamicRHI->CreateBuffer(createInfo);

    if (pData != nullptr)
    {
        InitializeBufferData(pStorageBuffer, dataSize, pData);
    }

    m_buffers.push_back(pStorageBuffer);

    return pStorageBuffer;
}

RHIBuffer* RenderDevice::CreateIndirectBuffer(uint32_t dataSize,
                                              const uint8_t* pData,
                                              NameID bufferName)
{
    BitField<RHIBufferUsageFlagBits> usages;
    usages.SetFlag(RHIBufferUsageFlagBits::eStorageBuffer);
    usages.SetFlag(RHIBufferUsageFlagBits::eIndirectBuffer);
    usages.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);

    uint32_t paddedSize = PadStorageBufferSize(dataSize);

    RHIBufferCreateInfo createInfo{};
    createInfo.size         = paddedSize;
    createInfo.usageFlags   = usages;
    createInfo.allocateType = RHIBufferAllocateType::eGPU;
    createInfo.tag          = bufferName;

    RHIBuffer* pIndirectBuffer = GDynamicRHI->CreateBuffer(createInfo);

    if (pData != nullptr)
    {
        InitializeBufferData(pIndirectBuffer, dataSize, pData);
    }

    m_buffers.push_back(pIndirectBuffer);

    return pIndirectBuffer;
}

size_t RenderDevice::PadUniformBufferSize(size_t size)
{
    return AlignBufferSize(size, GDynamicRHI->QueryGPUInfo().uniformBufferAlignment);
}

size_t RenderDevice::PadStorageBufferSize(size_t size)
{
    return AlignBufferSize(size, GDynamicRHI->QueryGPUInfo().storageBufferAlignment);
}

RHISampler* RenderDevice::CreateSampler(const RHISamplerCreateInfo& samplerInfo)
{
    const size_t samplerHash = CalcSamplerHash(samplerInfo);

    if (!m_samplerCache.contains(samplerHash))
    {
        m_samplerCache[samplerHash] = GDynamicRHI->CreateSampler(samplerInfo);
    }

    return m_samplerCache[samplerHash];
}

RHITexture* RenderDevice::LoadTexture2D(const std::string& file, bool requireMipmap)
{
    return m_pTextureManager->LoadTexture2D(file, requireMipmap);
}

void RenderDevice::LoadSceneTextures(const sg::Scene* pScene, std::vector<RHITexture*>& outTextures)
{
    m_pTextureManager->LoadSceneTextures(pScene, outTextures);
}

void RenderDevice::LoadTextureEnv(const std::string& file, EnvTexture* pTexture)
{
    std::string fullPath = ZEN_TEXTURE_PATH + file;
    m_pTextureManager->LoadTextureEnv(fullPath, pTexture);
}

size_t RenderDevice::CalcSamplerHash(const RHISamplerCreateInfo& info)
{
    using namespace zen::util;

    std::size_t seed = 0;

    HashCombine(seed, std::hash<int>()(static_cast<int>(info.magFilter)));
    HashCombine(seed, std::hash<int>()(static_cast<int>(info.minFilter)));
    HashCombine(seed, std::hash<int>()(static_cast<int>(info.mipFilter)));

    HashCombine(seed, std::hash<int>()(static_cast<int>(info.repeatU)));
    HashCombine(seed, std::hash<int>()(static_cast<int>(info.repeatV)));
    HashCombine(seed, std::hash<int>()(static_cast<int>(info.repeatW)));

    HashCombine(seed, std::hash<float>()(info.lodBias));
    HashCombine(seed, std::hash<bool>()(info.useAnisotropy));
    HashCombine(seed, std::hash<float>()(info.maxAnisotropy));
    HashCombine(seed, std::hash<bool>()(info.enableCompare));
    HashCombine(seed, std::hash<int>()(static_cast<int>(info.compareOp)));

    HashCombine(seed, std::hash<float>()(info.minLod));
    HashCombine(seed, std::hash<float>()(info.maxLod));

    HashCombine(seed, std::hash<int>()(static_cast<int>(info.borderColor)));
    HashCombine(seed, std::hash<bool>()(info.unnormalizedUVW));

    return seed;
}
} // namespace zen::rc
