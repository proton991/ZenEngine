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
#include <filesystem>

zen::rc::RenderFrameState GRenderFrameState;

namespace zen::rc
{
namespace
{
RHITextureCreateInfo MakeTextureInfo(const TextureFormat& format, TextureUsageHint hint, NameID name)
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
        info.usageFlags.SetFlags(RHITextureUsageFlagBits::eTransferSrc, RHITextureUsageFlagBits::eTransferDst);
    }

    return info;
}

size_t AlignBufferSize(size_t size, size_t alignment)
{
    alignment = std::max(size_t(1), alignment);

    VERIFY_EXPR_MSG(size <= std::numeric_limits<uint32_t>::max() - (alignment - 1), "Buffer size overflow");

    return (size + alignment - 1) / alignment * alignment;
}
} // namespace

RenderDevice::RenderDevice(RHIAPIType       APIType,
                           uint32_t         numFrames,
                           RHIExecutionMode executionMode,
                           AsyncComputeMode asyncComputeMode) :
    m_APIType(APIType),
    m_numFrames(std::clamp(numFrames, 1u, RenderFrameState::kMaxFramesInFlight)),
    m_executionMode(executionMode),
    m_asyncComputeMode(asyncComputeMode),
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

    m_pRHIExecutor       = ZEN_NEW() RHICommandListExecutor(GDynamicRHI, executionMode);

    GDynamicRHI          = m_pRHIExecutor;

    m_queueCapabilities  = m_pRHIExecutor->GetQueueCapabilities();

    m_asyncComputeStatus = ResolveAsyncComputeStatus(m_asyncComputeMode, m_queueCapabilities);

    GetRHIThread().Invoke(&RHIFrameState::Init, &GRHIFrameState, m_numFrames);

    m_pRHIDebug = RHIDebug::Create();
}

void RenderDevice::Init(RHIViewport* viewport)
{
    VERIFY_EXPR_MSG(m_pUploadQueue == nullptr, "RenderDevice is already initialized");

    LOGI("Async compute: requested={}; supported={}; compute/graphics={}; compute/transfer={}; policy={}; scheduling={}",
         m_asyncComputeMode == AsyncComputeMode::eAuto ? "auto" : "off",
         m_queueCapabilities.SupportsAsyncCompute() ? "yes" : "no",
         m_queueCapabilities.AreQueuesShared(RHICommandContextType::eAsyncCompute, RHICommandContextType::eGraphics)
             ? "shared"
             : "separate",
         m_queueCapabilities.AreQueuesShared(RHICommandContextType::eAsyncCompute, RHICommandContextType::eTransfer)
             ? "shared"
             : "separate",
         GetAsyncComputeStatusReason(m_asyncComputeStatus),
         m_asyncComputeStatus == AsyncComputeStatus::eAvailable ? "enabled for opted-in passes" : "graphics fallback");

    const bool asyncDependencies = GDynamicRHI->SupportsAsyncSubmissionDependencies();

    const bool sharedTransfer    = GDynamicRHI->IsTransferQueueSharedWithGraphics();

    LOGI("Upload/transfer support: async GPU dependencies={}; transfer queue={}; async transfer={}",
         asyncDependencies ? "yes" : "no", sharedTransfer ? "shared with graphics" : "separate",
         sharedTransfer      ? "unavailable (shared graphics queue)"
         : asyncDependencies ? "enabled (awaiting first use)"
                             : "disabled (CPU completion wait fallback)");

    m_pImmediateTransferCmdList = RHICommandList::Create(GDynamicRHI->GetTransferCommandContext());

    m_pUploadQueue              = ZEN_NEW() StagingUploadQueue(this, &m_stagingBufferManager);

    m_pTextureManager           = ZEN_NEW() TextureManager(this, m_pUploadQueue);

    m_frameRDG                  = MakeUnique<RenderGraph>("frame_rdg");

    // Each frame slot alternates between its own transient set, so the working set spans
    // every slot; keeping it pooled avoids recreating render targets and their descriptors.
    RDGPoolConfig framePool;

    framePool.steadyBuilds = m_numFrames;

    VERIFY_EXPR_MSG(m_frameRDG->GetResourceManager()->SetPoolConfig(framePool), "Failed to configure the frame graph pool");

    m_pMainViewport = viewport;

    BeginFrame();
}

void RenderDevice::InitializeRendererServer()
{
    VERIFY_EXPR_MSG(m_pUploadQueue != nullptr, "Initialize the render device before its renderer server");

    VERIFY_EXPR_MSG(m_pRendererServer == nullptr, "RendererServer is already initialized");

    m_pRendererServer = ZEN_NEW() RendererServer(this, m_pMainViewport);

    m_pRendererServer->Init();
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

        m_computeCmdListPool.Destroy();

        m_transferCmdListPool.Destroy();

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

        m_submissionHistory.Clear();

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

    if (m_executionMode == RHIExecutionMode::eThreaded || m_asyncComputeStatus == AsyncComputeStatus::eAvailable)
    {
        result = ExecuteFrameGraph(viewport);
    }
    else if ((((viewport != nullptr) && (m_frameRDG.Get() != nullptr)) && (m_pImmediateTransferCmdList != nullptr))
             && (!m_frameWaitFailed))
    {
        RenderGraph& graph = *m_frameRDG;

        if (m_submissionBlocked)
        {
            result =
                graph.Fail(RDGErrorCode::eSubmission, "GPU execution is blocked; recreate the device after submission failure");
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
                result = graph.Fail(RDGErrorCode::eSubmission, "Pending uploads failed before frame execution");
            }
            else if (!m_rdgExecutor.RefreshExecution(plan))
            {
                result = false;
            }
            else
            {
                // The executor submits the presentation copy with the graph, so a rejected
                // submission accepts neither and keeps the acquired image for a retry.
                RHICommandList* commands = m_graphicsCmdListPool.Acquire();

                bool presented           = false;

                const bool executed =
                    commands != nullptr
                    && m_rdgExecutor.ExecutePrepared(plan, commands,
                                                     std::bind_front(&RenderDevice::SubmitRecordedGraph, this, std::ref(graph),
                                                                     std::ref(*commands), viewport, nullptr, &presented));

                m_graphicsCmdListPool.Release(commands);

                if (executed)
                {
                    m_rdgExecutor.GetResourceStateTracker().UpdateTextureState(
                        viewport->GetColorBackBuffer(), RHIAccessMode::eReadWrite, RHITextureUsage::eColorAttachment,
                        BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eColorAttachmentOutput));

                    EndFrame();

                    result = presented && !m_frameActive;
                }
            }
        }
    }

    return result;
}

bool RenderDevice::ExecuteFrameGraph(RHIViewport* viewport)
{
    bool result = false;

    PollFrameSubmissions(false);

    if (viewport != nullptr && m_frameRDG.Get() != nullptr && m_frameActive && !m_frameWaitFailed && !AreSubmissionsBlocked()
        && m_pRecreateViewport == nullptr)
    {
        RenderGraph& graph = *m_frameRDG;

        RDGExecutor::ExecutionPlan plan;

        if (m_rdgExecutor.PrepareExecution(&graph, plan) && (m_pUploadQueue == nullptr || m_pUploadQueue->Flush())
            && m_rdgExecutor.RefreshExecution(plan))
        {
            PendingFrame pending;

            pending.frame    = &m_frames[ToIndex(GRenderFrameState.GetFrameSlot())];

            pending.viewport = viewport;

            if (m_asyncComputeStatus == AsyncComputeStatus::eAvailable)
            {
                result = ExecuteScheduledGraph(plan, viewport, &pending);
            }
            else
            {
                RHICommandList* commands = m_graphicsCmdListPool.Acquire();

                result =
                    commands != nullptr
                    && m_rdgExecutor.ExecutePrepared(plan, commands,
                                                     std::bind_front(&RenderDevice::SubmitRecordedGraph, this, std::ref(graph),
                                                                     std::ref(*commands), viewport, &pending, nullptr),
                                                     true);

                m_graphicsCmdListPool.Release(commands);
            }

            if (result)
            {
                // Scheduled state belongs to RenderCore. Confirmation comes back through
                // the ticket; the worker never touches this graph or its pass callbacks.
                m_rdgExecutor.GetResourceStateTracker().UpdateTextureState(
                    viewport->GetColorBackBuffer(), RHIAccessMode::eReadWrite, RHITextureUsage::eColorAttachment,
                    BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eColorAttachmentOutput));
            }

            if (pending.ticket.IsValid())
            {
                // Handoff owns the frame even if inline execution or history commit failed.
                // Always consume its result so accepted serials reach frame retirement.
                pending.frame->submission = pending.ticket;

                m_pendingFrames.push_back(std::move(pending));

                // The queued frame batch owns native EndFrame, including on the slow path.
                m_frameActive = false;

                if (m_executionMode == RHIExecutionMode::eInline)
                {
                    const RHIBatchResult native = m_pendingFrames.back().ticket.Wait();

                    const bool confirmed        = PollFrameSubmissions(true);

                    result = result && confirmed && native.submission == RHISubmissionResult::eSuccess && native.presented;
                }
            }
        }
    }

    CollectDestroyedResourceHistory();

    return result;
}

void RenderDevice::CompleteFrame(PendingFrame& pending, const RHIBatchResult& result)
{
    RenderFrame& frame = *pending.frame;

    frame.retirement.requiredSerials.Extend(result.requiredSerials);

    frame.submission = {};

    if (result.submission == RHISubmissionResult::eSuccess && !m_submissionBlocked && m_submissionHistory.ResolveAccepted())
    {
        LogAsyncComputeSubmission(pending.graphName, pending.computePassCount, result);

        for (const RDGDeferredExtraction& extraction : pending.extractions)
        {
            if (extraction.state->resource == nullptr)
            {
                extraction.resource->AddReference();

                extraction.state->resource = extraction.resource.Get();

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

        m_submissionHistory.Clear();

        if (!result.cause.IsFailure())
        {
            LOGE("RHI submission history could not be confirmed; rendering stopped");
        }
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
            m_submissionHistory.Erase(resourceId);

            m_rdgExecutor.GetResourceStateTracker().RemoveResourceState(resourceId);
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

bool RenderDevice::CanReconfigureResources() const
{
    const RDGExecutionState state = m_frameRDG.Get() != nullptr ? m_frameRDG->GetExecutionState() : RDGExecutionState::eIdle;

    return !AreSubmissionsBlocked() && state != RDGExecutionState::eBuilding && state != RDGExecutionState::eRecorded
        && state != RDGExecutionState::eExecuting;
}

bool RenderDevice::PrepareForResourceReconfiguration()
{
    bool valid = CanReconfigureResources();

    if (valid)
    {
        if (m_pUploadQueue != nullptr)
        {
            valid = m_pUploadQueue->Flush();
        }

        if (valid)
        {
            WaitForPreviousFrames();
        }

        valid = valid && !AreSubmissionsBlocked();

        if (valid && m_pUploadQueue != nullptr)
        {
            // Completion removes the staging queue's borrowed references before scene owners
            // and the upload graph's deferred imports are retired at this boundary.
            m_pUploadQueue->ReclaimResources();
        }

        if (valid && m_frameRDG.Get() != nullptr)
        {
            valid = m_frameRDG->Reset();

            m_rdgPassCompiler.SetRenderGraph(nullptr);
        }

        if (valid)
        {
            CollectCompletedResources();
        }
    }

    return valid;
}

bool RenderDevice::PrepareForSceneReplacement()
{
    const bool prepared = PrepareForResourceReconfiguration() && GDynamicRHI->ResetBindlessResources();

    return prepared;
}

bool RenderDevice::SetAsyncComputeMode(AsyncComputeMode mode)
{
    bool valid = CanReconfigureResources() && (mode == AsyncComputeMode::eDisabled || mode == AsyncComputeMode::eAuto);

    if (valid && mode != m_asyncComputeMode)
    {
        valid = PrepareForResourceReconfiguration();

        if (valid)
        {
            m_asyncComputeMode             = mode;

            m_asyncComputeStatus           = ResolveAsyncComputeStatus(mode, m_queueCapabilities);

            m_loggedAsyncComputeSubmission = false;

            LOGI("Runtime async compute: {}", GetAsyncComputeStatusReason(m_asyncComputeStatus));
        }
    }

    return valid;
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
            graph.Fail(RDGErrorCode::eSubmission, "GPU execution is blocked; recreate the device after submission failure");
    }
    else if (m_frameWaitFailed)
    {
        LOGE("RenderDevice: graph execution blocked until the frame slot completes");

        result = false;
    }
    else if (m_pImmediateTransferCmdList == nullptr)
    {
        result = graph.Fail(RDGErrorCode::eLifecycle, "RenderDevice must be initialized before graph execution");
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
            result = graph.Fail(RDGErrorCode::eSubmission, "Pending uploads failed before graph execution");
        }
        else if (!m_rdgExecutor.RefreshExecution(plan))
        {
            result = false;
        }
        else if (m_asyncComputeStatus == AsyncComputeStatus::eAvailable)
        {
            result = ExecuteScheduledGraph(plan);
        }
        else
        {
            const bool transfer  = plan.transfer;

            RHICommandList* list = transfer ? m_pImmediateTransferCmdList : m_graphicsCmdListPool.Acquire();

            if (list == nullptr)
            {
                result = graph.Fail(RDGErrorCode::eLifecycle, "RenderDevice must be initialized before graph execution");
            }
            else
            {
                const bool executed =
                    m_rdgExecutor.ExecutePrepared(plan, list,
                                                  std::bind_front(&RenderDevice::SubmitRecordedGraph, this, std::ref(graph),
                                                                  std::ref(*list), nullptr, nullptr, nullptr));

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

bool RenderDevice::ExecuteScheduledGraph(RDGExecutor::ExecutionPlan& plan, RHIViewport* viewport, PendingFrame* pending)
{
    RenderSubmissionUpdate update;

    bool result = PrepareScheduledSubmissionHistory(*plan.graph, plan.schedule, update);

    if (result)
    {
        plan.schedule = std::move(update.schedule);

        HeapVector<RHICommandList*> lists;

        AcquireScheduledCmdLists(plan.schedule, lists);

        result = std::find(lists.begin(), lists.end(), nullptr) == lists.end()
              && m_rdgExecutor.ExecutePreparedGroups(
                  plan, lists, update.initialStates,
                  std::bind_front(&RenderDevice::SubmitRecordedGroups, this, std::ref(*plan.graph), std::cref(plan.schedule),
                                  std::ref(update), VectorView<RHICommandList*>(lists), viewport, pending, nullptr),
                  pending != nullptr);

        ReleaseScheduledCmdLists(lists);
    }

    return result;
}

RHISubmissionResult RenderDevice::SubmitRecordedGroups(RenderGraph&                graph,
                                                       const RDGSchedule&          schedule,
                                                       RenderSubmissionUpdate&     update,
                                                       VectorView<RHICommandList*> lists,
                                                       RHIViewport*                viewport,
                                                       PendingFrame*               pending,
                                                       bool*                       pPresented)
{
    if (pending != nullptr)
    {
        // The only render-thread backpressure point. Recording and group submission add no GPU wait.
        PollFrameSubmissions(true);
    }

    RHISubmissionResult result = RHISubmissionResult::eRejected;

    bool valid = !AreSubmissionsBlocked() && m_pRecreateViewport == nullptr && m_submissionHistory.CanCommit(update)
              && lists.size() == schedule.groups.size();

    HeapVector<RHISubmissionGroup> submissions;

    uint32_t computePassCount = 0;

    for (size_t i = 0; valid && i < schedule.groups.size(); ++i)
    {
        const RDGSubmissionGroup& source = schedule.groups[i];

        if (source.queue == RHICommandContextType::eAsyncCompute)
        {
            computePassCount += uint32_t(source.passes.size());
        }

        RHISubmissionGroup& submission = submissions.emplace_back();

        submission.commands            = lists[i];

        for (uint32_t id : source.predecessors)
        {
            valid &= id < i;

            if (valid)
            {
                submission.predecessors.push_back({schedule.groups[id].queue, 0, update.state, id});
            }
        }

        for (uint32_t id : source.externalPredecessors)
        {
            valid &= id < update.externalPoints.size();

            if (valid)
            {
                submission.predecessors.push_back(update.externalPoints[id]);
            }
        }
    }

    if (valid && pending != nullptr)
    {
        pending->graphName        = graph.m_rdgTag;

        pending->computePassCount = computePassCount;

        graph.m_resourceManager.StageExtractions(pending->extractions);

        pending->ticket = m_pRHIExecutor->SubmitFrame(submissions, viewport, update.state);

        if (pending->ticket.IsValid())
        {
            result = m_submissionHistory.Commit(update) ? RHISubmissionResult::eSuccess : RHISubmissionResult::eFatal;
        }
    }
    else if (valid)
    {
        const RHIBatchResult native = m_pRHIExecutor->SubmitGroups(submissions, update.state, viewport);

        StampOutgoingFrameSerials();

        result = native.submission;

        if (native.needsRecreation && viewport != nullptr)
        {
            m_pRecreateViewport = viewport;
        }

        if (pPresented != nullptr)
        {
            *pPresented = native.presented;
        }

        if (result == RHISubmissionResult::eSuccess && !m_queueCapabilities.asyncSubmissionDependencies
            && !m_queueCapabilities.AreQueuesShared(RHICommandContextType::eTransfer, RHICommandContextType::eGraphics))
        {
            // Preserve synchronous upload completion on backends without timeline waits.
            for (const RHISubmissionGroupResult& group : native.groups)
            {
                if (result == RHISubmissionResult::eSuccess && group.accepted.queue == RHICommandContextType::eTransfer
                    && !m_pRHIExecutor->WaitForCompletion(group.accepted.queue, group.accepted.serial))
                {
                    result = RHISubmissionResult::eFatal;
                }
            }
        }

        if (result == RHISubmissionResult::eSuccess)
        {
            if (m_submissionHistory.Commit(update) && m_submissionHistory.ResolveAccepted())
            {
                LogAsyncComputeSubmission(graph.m_rdgTag, computePassCount, native);

                for (const RHISubmissionGroupResult& group : native.groups)
                {
                    if (group.accepted.serial != 0)
                    {
                        LogTransferSubmission(graph, group.accepted.queue, group.accepted.serial);
                    }
                }
            }
            else
            {
                result = RHISubmissionResult::eFatal;
            }
        }
    }

    if (AreSubmissionsBlocked() || result == RHISubmissionResult::eFatal)
    {
        m_submissionBlocked = true;

        m_submissionHistory.Clear();

        result = RHISubmissionResult::eFatal;
    }

    return result;
}

void RenderDevice::LogAsyncComputeSubmission(NameID graphName, uint32_t computePassCount, const RHIBatchResult& result)
{
    if (!m_loggedAsyncComputeSubmission && computePassCount != 0 && result.submission == RHISubmissionResult::eSuccess)
    {
        uint64_t serial = 0;

        for (const RHISubmissionGroupResult& group : result.groups)
        {
            if (group.submission == RHISubmissionResult::eSuccess
                && group.accepted.queue == RHICommandContextType::eAsyncCompute)
            {
                serial = std::max(serial, group.accepted.serial);
            }
        }

        if (serial != 0)
        {
            LOGI("Async compute in use: graph={}; passes={}; compute serial={}", graphName.ToString(), computePassCount,
                 serial);

            m_loggedAsyncComputeSubmission = true;
        }
    }
}

bool RenderDevice::PrepareScheduledSubmissionHistory(const RenderGraph&      graph,
                                                     const RDGSchedule&      schedule,
                                                     RenderSubmissionUpdate& update,
                                                     bool                    serializeReads)
{
    HeapVector<HeapVector<RenderSubmissionAccess>> accesses(schedule.groups.size());

    m_submissionBlocked |= !m_submissionHistory.ResolveAccepted();

    bool valid           = !AreSubmissionsBlocked();

    for (size_t groupIndex = 0; valid && groupIndex < schedule.groups.size(); ++groupIndex)
    {
        for (const RDGScheduledPass& pass : schedule.groups[groupIndex].passes)
        {
            valid                   = pass.nodeId.IsValid() && uint32_t(pass.nodeId) < graph.m_nodes.size();

            const RDGNodeBase* node = valid ? graph.GetNodeBaseById(pass.nodeId) : nullptr;

            for (uint32_t i = 0; valid && i < node->accessCount; ++i)
            {
                const RDGAccess& access                        = graph.m_accesses[node->accessOffset + i];

                const RDGResourceManager::Allocation* resource = graph.m_resourceManager.FindResourceByIdx(access.resourceId);

                const RHIResource* physical                    = resource == nullptr ? nullptr
                                                               : resource->type == RDGResourceType::eTexture
                                                                   ? static_cast<const RHIResource*>(resource->pTexture)
                                                                   : resource->pBuffer;

                valid                                          = physical != nullptr;

                if (valid)
                {
                    accesses[groupIndex].push_back({physical->GetStableId(), resource->type == RDGResourceType::eTexture,
                                                    physical->IsAsyncComputeAccessible(), access});
                }
            }

            if (!valid)
            {
                break;
            }
        }
    }

    valid = valid && m_submissionHistory.Prepare(schedule, accesses, m_queueCapabilities, update, serializeReads);

    return valid;
}

bool RenderDevice::PrepareGraphSubmission(const RenderGraph& graph, RHICommandList& commands, RenderSubmissionUpdate& update)
{
    RDGSchedule schedule;

    RDGSubmissionGroup& group = schedule.groups.emplace_back();

    group.queue               = commands.GetContext()->GetContextType();

    group.queueEquivalenceId  = m_queueCapabilities.queueIds[size_t(group.queue)];

    for (const RDGCompiledNode& compiled : graph.m_compiledNodes)
    {
        group.passes.push_back({compiled.nodeId});
    }

    // The disabled/unsupported policy retains the conservative single-list fallback.
    const bool valid = PrepareScheduledSubmissionHistory(graph, schedule, update, true);

    return valid;
}

RHISubmissionResult RenderDevice::SubmitRecordedGraph(RenderGraph&    graph,
                                                      RHICommandList& commands,
                                                      RHIViewport*    viewport,
                                                      PendingFrame*   pending,
                                                      bool*           pPresented)
{
    RenderSubmissionUpdate update;

    RHISubmissionResult result = RHISubmissionResult::eRejected;

    if (PrepareGraphSubmission(graph, commands, update))
    {
        RHICommandList* list = &commands;

        result = SubmitRecordedGroups(graph, update.schedule, update, MakeVecView(&list, 1), viewport, pending, pPresented);
    }
    else if (AreSubmissionsBlocked())
    {
        result = RHISubmissionResult::eFatal;
    }

    return result;
}

void RenderDevice::LogTransferSubmission(const RenderGraph& graph, RHICommandContextType queue, uint64_t serial)
{
    // Report actual accepted work once per route, rather than every upload or frame.
    if (queue == RHICommandContextType::eTransfer && !m_loggedTransferSubmission)
    {
        const char* mode = GDynamicRHI->IsTransferQueueSharedWithGraphics()
                             ? "shared graphics queue (no transfer/graphics overlap)"
                         : GDynamicRHI->SupportsAsyncSubmissionDependencies()
                             ? "async transfer (GPU timeline dependencies; no CPU completion wait)"
                             : "synchronous transfer (CPU completion wait)";

        LOGI("Upload/transfer in use: {}; first submission serial={}", mode, serial);

        m_loggedTransferSubmission = true;
    }
    else if (queue == RHICommandContextType::eGraphics && !m_loggedGraphicsTransferSubmission
             && !graph.m_compiledXferPasses.empty() && graph.m_compiledGfxPasses.empty()
             && graph.m_compiledComputePasses.empty())
    {
        LOGI("Upload/transfer in use: graphics queue fallback; first submission serial={}", serial);

        m_loggedGraphicsTransferSubmission = true;
    }
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
        m_submissionHistory.Erase(texture->GetStableId());

        m_rdgExecutor.GetResourceStateTracker().RemoveResourceState(texture->GetStableId(), true);
    }
}

void RenderDevice::InvalidateExternalBufferState(RHIBuffer* buffer)
{
    if (buffer != nullptr)
    {
        m_submissionHistory.Erase(buffer->GetStableId());

        m_rdgExecutor.GetResourceStateTracker().RemoveResourceState(buffer->GetStableId(), true);
    }
}

bool RenderDevice::ResolveStagingFlushAction(StagingFlushAction action, StagingBufferManager* manager)
{
    bool returnValue{};

    if (action == StagingFlushAction::eNone)
    {
        returnValue = true;
    }
    else if (action == StagingFlushAction::eFailed || m_resolvingStagingFlush)
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

        m_submissionBlocked  |= result == RHISubmissionResult::eFatal;

        const bool completed  = !m_submissionBlocked && result == RHISubmissionResult::eSuccess
                            && (manager != nullptr ? manager : &m_stagingBufferManager)->WaitForSubmittedAllocations();

        m_resolvingStagingFlush = false;

        returnValue             = completed;
    }

    return returnValue;
}

RHIRenderingLayout* RenderDevice::AcquireRenderingLayout()
{
    RHIRenderingLayout* layout = m_renderingLayoutPool.empty() ? ZEN_NEW() RHIRenderingLayout() : m_renderingLayoutPool.back();

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
    RHIRenderingLayout** it = std::find(m_renderingLayoutPool.begin(), m_renderingLayoutPool.end(), layout);

    if (it != m_renderingLayoutPool.end())
    {
        m_renderingLayoutPool.erase(it);
    }

    ZEN_DELETE(layout);
}

RHITexture* RenderDevice::CreateTextureColorRT(const TextureFormat& format, TextureUsageHint hint, NameID name)
{
    RHITextureCreateInfo info = MakeTextureInfo(format, hint, name);

    info.usageFlags.SetFlags(RHITextureUsageFlagBits::eColorAttachment, RHITextureUsageFlagBits::eSampled);

    return GDynamicRHI->CreateTexture(info);
}

RHITexture* RenderDevice::CreateTextureDepthStencilRT(const TextureFormat& format, TextureUsageHint hint, NameID name)
{
    RHITextureCreateInfo info = MakeTextureInfo(format, hint, name);

    info.usageFlags.SetFlags(RHITextureUsageFlagBits::eDepthStencilAttachment, RHITextureUsageFlagBits::eSampled);

    return GDynamicRHI->CreateTexture(info);
}

RHITexture* RenderDevice::CreateTextureStorage(const TextureFormat& format, TextureUsageHint hint, NameID name)
{
    RHITextureCreateInfo info = MakeTextureInfo(format, hint, name);

    info.usageFlags.SetFlags(RHITextureUsageFlagBits::eStorage, RHITextureUsageFlagBits::eSampled);

    return GDynamicRHI->CreateTexture(info);
}

RHITexture* RenderDevice::CreateTextureSampled(const TextureFormat& format, TextureUsageHint hint, NameID name)
{
    RHITextureCreateInfo info = MakeTextureInfo(format, hint, name);

    info.usageFlags.SetFlag(RHITextureUsageFlagBits::eSampled);

    return GDynamicRHI->CreateTexture(info);
}

RHITexture* RenderDevice::CreateTextureDummy(const TextureFormat& format, TextureUsageHint hint, NameID name)
{
    return CreateTextureSampled(format, hint, name);
}

RHITextureView* RenderDevice::CreateTextureView(RHITexture* texture, const TextureViewFormat& format, NameID name)
{
    VERIFY_EXPR_MSG(texture != nullptr, "Cannot create a view of a null texture");

    RHITextureViewCreateInfo info{};

    info.format       = format.format == DataFormat::eUndefined ? texture->GetFormat() : format.format;

    info.type         = static_cast<RHITextureType>(format.dimension);

    info.arrayLayers  = format.arrayLayers;

    info.mipLevels    = format.mipmaps;

    info.baseMipLevel = format.baseMipLevel;

    info.tag          = name;

    VERIFY_EXPR_MSG(info.arrayLayers > 0 && info.arrayLayers <= texture->GetArrayLayers() && info.mipLevels > 0
                        && uint64_t(info.baseMipLevel) + info.mipLevels <= texture->GetNumMipmaps(),
                    "Invalid texture view range");

    return GDynamicRHI->CreateTextureView(texture, info);
}

void RenderDevice::DestroyTexture(RHITexture* texture)
{
    if (texture != nullptr)
    {
        if (m_pUploadQueue != nullptr)
        {
            m_pUploadQueue->Flush();
        }

        m_frames[GetCurrentFrameSlot()].texturesPendingFree.push_back(texture);

        StampOutgoingFrameSerials();
    }
}

void RenderDevice::UpdateBuffer(RHIBuffer* buffer, uint32_t size, const uint8_t* data, uint32_t offset)
{
    UpdateBufferInternal(buffer, offset, size, data);
}

void RenderDevice::UpdateTexture(RHITexture*                            texture,
                                 VectorView<RHIBufferTextureCopyRegion> regions,
                                 uint32_t                               dataSize,
                                 const uint8_t*                         data)
{
    VERIFY_EXPR_MSG(m_pUploadQueue != nullptr, "RenderDevice must be initialized before texture uploads");

    m_pUploadQueue->EnqueueTexture(texture, regions, dataSize, data);
}

RHIBuffer* RenderDevice::CreateInitializedBuffer(const RHIBufferCreateInfo& info,
                                                 uint32_t                   dataSize,
                                                 const uint8_t*             data,
                                                 bool                       padData)
{
    RHIBuffer* buffer = CreateBuffer(info);

    if (buffer != nullptr)
    {
        m_buffers.push_back(buffer);

        const bool initialized =
            padData ? InitializeBufferData(buffer, dataSize, data) : UpdateBufferInternal(buffer, 0, dataSize, data);

        if (!initialized)
        {
            DestroyBuffer(buffer);

            buffer = nullptr;
        }
    }

    return buffer;
}

bool RenderDevice::InitializeBufferData(RHIBuffer* buffer, uint32_t dataSize, const uint8_t* data)
{
    bool initialized = buffer != nullptr;

    if (initialized && data != nullptr && dataSize > 0)
    {
        const uint32_t size = static_cast<uint32_t>(buffer->GetRequiredSize());

        // Assemble only the final aligned word and padding, never read beyond caller data.
        const uint32_t prefix = dataSize & ~uint32_t(3);

        if (prefix != 0)
        {
            initialized = UpdateBufferInternal(buffer, 0, prefix, data);
        }

        if (initialized && prefix < size)
        {
            HeapVector<uint8_t> tail(size - prefix);

            std::memcpy(tail.data(), data + prefix, dataSize - prefix);

            initialized = UpdateBufferInternal(buffer, prefix, static_cast<uint32_t>(tail.size()), tail.data());
        }
    }

    return initialized;
}

bool RenderDevice::UpdateBufferInternal(RHIBuffer* buffer, uint32_t offset, uint32_t size, const uint8_t* data)
{
    bool initialized = buffer != nullptr;

    if (initialized && data != nullptr && size > 0)
    {
        VERIFY_EXPR_MSG(m_pUploadQueue != nullptr, "RenderDevice must be initialized before buffer uploads");

        initialized = m_pUploadQueue->EnqueueBuffer(buffer, offset, size, data);
    }

    return initialized;
}

void RenderDevice::DestroyBuffer(RHIBuffer* buffer)
{
    if (buffer != nullptr)
    {
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

RHIPipeline* RenderDevice::GetOrCreateGfxPipeline(const RHIGfxPipelineStates&                            states,
                                                  RHIShader*                                             shader,
                                                  const RHIRenderingLayout*                              layout,
                                                  const HashMap<uint32_t, RHIShaderSpecializationValue>& constants)
{
    const RDGMetricsOptions& options = GetRDGMetrics().GetOptions();

    return GetOrCreateGfxPipeline(states, shader, layout, constants, options.logging.enabled && options.preparationTimings);
}

RHIPipeline* RenderDevice::GetOrCreateGfxPipeline(const RHIGfxPipelineStates&                            states,
                                                  RHIShader*                                             shader,
                                                  const RHIRenderingLayout*                              layout,
                                                  const HashMap<uint32_t, RHIShaderSpecializationValue>& constants,
                                                  bool                                                   timed)
{
    RHIPipeline* pipeline = nullptr;

    if (shader != nullptr && layout != nullptr)
    {
        if (layout->numColorRenderTargets > MAX_NUM_COLOR_ATTACHMENTS
            || states.colorBlendState.attachmentIdx > MAX_NUM_COLOR_ATTACHMENTS)
        {
            LOGE("Pipeline cache [{}]: attachment count exceeds the supported limit", uint32_t(RDGErrorCode::eRange));
        }
        else
        {
            ++m_pipelineMetrics.requests;

            m_pipelineMetrics.timedRequests += timed;

            ScopedMetricsTimer keyTimer(timed, m_pipelineMetrics.keyCPUUs);

            PipelineKey key = MakePipelineKey(shader, &states, layout, constants);

            keyTimer.Stop();

            ScopedMetricsTimer lookupTimer(timed, m_pipelineMetrics.lookupCPUUs);

            LRUCache<PipelineKey, RHIPipeline*, PipelineKeyHasher>::iterator it = m_pipelineCache.find(key);

            lookupTimer.Stop();

            if (it != m_pipelineCache.end())
            {
                ++m_pipelineMetrics.hits;

                pipeline = it->second;
            }
            else if (IsPipelineRetryDeferred(key))
            {
                ++m_pipelineMetrics.misses;

                ++m_pipelineMetrics.failures;
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

                    for (const HashMap<uint32_t, RHIShaderSpecializationValue>::value_type& constant : constants)
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

                    pipeline              = GDynamicRHI->CreatePipeline(info);

                    if (pipeline != nullptr)
                    {
                        ++m_pipelineMetrics.creations;

                        creationTimer.Stop();

                        m_failedPipelines.erase(key);

                        m_pipelineCache.try_emplace(std::move(key), pipeline);
                    }
                }

                if (pipeline == nullptr)
                {
                    ++m_pipelineMetrics.failures;

                    RecordPipelineFailure(key);
                }
            }
        }
    }

    return pipeline;
}

RHIPipeline* RenderDevice::GetOrCreateComputePipeline(RHIShader* shader)
{
    const RDGMetricsOptions& options = GetRDGMetrics().GetOptions();

    return GetOrCreateComputePipeline(shader, options.logging.enabled && options.preparationTimings);
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
        else if (IsPipelineRetryDeferred(key))
        {
            ++m_pipelineMetrics.misses;

            ++m_pipelineMetrics.failures;
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

                RecordPipelineFailure(key);

                result = nullptr;
            }
            else
            {
                ++m_pipelineMetrics.creations;

                creationTimer.Stop();

                m_failedPipelines.erase(key);

                m_pipelineCache.try_emplace(std::move(key), pipeline);

                result = pipeline;
            }
        }
    }

    return result;
}

bool RenderDevice::IsPipelineRetryDeferred(const PipelineKey& key)
{
    PipelineFailureCache::iterator it = m_failedPipelines.find(key);

    return it != m_failedPipelines.end() && ToValue(GRenderFrameState.GetFrameNumber()) < it->second.retryFrame;
}

void RenderDevice::RecordPipelineFailure(const PipelineKey& key)
{
    PipelineFailure& failure = m_failedPipelines[key];

    // A first failure may be transient, so the next request retries at once. Each further
    // failure doubles the frame delay, up to 2^kMaxPipelineRetryShift frames.
    const uint64_t delay = failure.consecutive == 0 ? 0 : uint64_t(1) << std::min(failure.consecutive, kMaxPipelineRetryShift);

    ++failure.consecutive;

    failure.retryFrame = ToValue(GRenderFrameState.GetFrameNumber()) + delay;
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

        HeapVector<RHIViewport*>::iterator it = std::find(m_viewports.begin(), m_viewports.end(), viewport);

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
    if (viewport != nullptr && width != 0 && height != 0
        && (viewport->GetWidth() != width || viewport->GetHeight() != height
            || GetRHIThread().Invoke(&RHIViewport::NeedsRecreation, viewport)))
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

            viewport->Resize(width, height);

            m_pRecreateViewport = nullptr;
        }
    }
}

void RenderDevice::ProcessViewportResize(uint32_t width, uint32_t height)
{
    if ((width != 0) && (height != 0))
    {
        ResizeViewport(m_pMainViewport, width, height);
    }
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
    RenderFrame& frame        = m_frames[GetCurrentFrameSlot()];

    ResourceRetirement latest = CaptureResourceRetirement();

    frame.retirement.requiredSerials.Extend(latest.requiredSerials);

    frame.retirement.pending = std::move(latest.pending);
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
    if (ignoreCompletionGate || (!AreSubmissionsBlocked() && !m_frames[ToIndex(slot)].submission.IsValid()))
    {
        RenderFrame& frame = m_frames[ToIndex(slot)];

        if (ignoreCompletionGate || IsResourceRetired(frame.retirement))
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

            frame.retirement = {};
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

        if ((!m_frameActive) && (!m_submissionBlocked))
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

        RenderFrame& frame         = m_frames[ToIndex(slot)];

        if (frame.submission.IsValid() || frame.retirement.pending.IsValid())
        {
            PollFrameSubmissions(true);
        }

        if (frame.retirement.pending.IsValid())
        {
            frame.retirement.pending.Wait();
        }

        bool ready = !AreSubmissionsBlocked() && frame.retirement.Resolve();

        for (size_t i = 0; i < RHICompletionSet::kQueueCount; ++i)
        {
            const RHICommandContextType queue = static_cast<RHICommandContextType>(i);

            const uint64_t serial             = frame.retirement.requiredSerials.Get(queue);

            if (ready && GDynamicRHI->QueryLastCompletedSerial(queue) < serial
                && !GDynamicRHI->WaitForCompletion(queue, serial))
            {
                LOGE("RenderDevice: frame slot {} cannot reuse queue {} serial {}", ToIndex(slot), uint32_t(queue), serial);

                m_frameWaitFailed = true;

                ready             = false;
            }
        }

        ready = ready && !AreSubmissionsBlocked();

        if (ready)
        {
            if (m_pUploadQueue != nullptr)
            {
                m_pUploadQueue->ReclaimResources();
            }

            ProcessPendingFreeResources(slot);

            m_stagingBufferManager.Reclaim();

            // Only evict cached graph resources at a frame boundary. In-flight,
            // exported and active resources keep their existing serial protection.
            // The working set of every frame slot stays pooled: evicting it would only rebuild
            // the same targets next frame while the retired copies still await their serials.
            if (m_frameRDG && GetGPUMemoryStats().IsUnderPressure())
            {
                m_frameRDG->GetResourceManager()->TrimIdlePoolEntries();
            }

            GDynamicRHI->BeginFrame();

            m_frameActive     = true;

            m_frameWaitFailed = false;
        }
    }
}

RHIBuffer* RenderDevice::CreateBuffer(const RHIBufferCreateInfo& info)
{
    return m_pRHIExecutor->CreateBuffer(info);
}

RHIQueueCopyCapabilities RenderDevice::GetQueueCopyCapabilities(RHICommandContextType queue)
{
    return GDynamicRHI != nullptr ? GDynamicRHI->GetQueueCopyCapabilities(queue) : RHIQueueCopyCapabilities{};
}

RHITextureCopyCapabilities RenderDevice::GetTextureCopyCapabilities(DataFormat format)
{
    return GDynamicRHI != nullptr ? GDynamicRHI->GetTextureCopyCapabilities(format) : RHITextureCopyCapabilities{};
}

RHITexture* RenderDevice::CreateTexture(const RHITextureCreateInfo& info)
{
    return m_pRHIExecutor->CreateTexture(info);
}

RHICompletionSet RenderDevice::GetSubmittedSerials() const
{
    return m_pRHIExecutor->GetSubmittedSerials();
}

RHICompletionSet RenderDevice::GetCachedCompletedSerials() const
{
    return m_pRHIExecutor->GetCachedCompletedSerials();
}

ResourceRetirement RenderDevice::CaptureResourceRetirement()
{
    ResourceRetirement result;

    // RDG preparation/recording only reads published progress; polling is owned by
    // frame boundaries, submissions, explicit collection, and completion waits.
    result.requiredSerials = m_pRHIExecutor->GetCachedSubmittedSerials();

    if (m_pendingFrames.size() > 1)
    {
        // RenderDevice drains the previous frame at handoff. Never discard an
        // unexpected second ticket and then permit resource reuse in release builds.
        m_submissionBlocked = true;

        LOGE("RenderDevice: more than one pending frame; resource reuse is blocked");
    }
    else if (!m_pendingFrames.empty())
    {
        result.pending = m_pendingFrames[0].ticket;
    }

    return result;
}

bool RenderDevice::IsResourceRetired(const ResourceRetirement& requirement) const
{
    return !AreSubmissionsBlocked() && requirement.IsCompleteAt(GetCachedCompletedSerials());
}

RDGAsyncComputeEligibility RenderDevice::ResolveAsyncComputeEligibility(const RenderGraph& graph, const RDGPassNode& node) const
{
    RDGAsyncComputeEligibility result = RDGAsyncComputeEligibility::eNotRequested;

    if (node.type == RDGNodeType::eGraphicsPass)
    {
        result = RDGAsyncComputeEligibility::eGraphicsPass;
    }
    else if (node.queuePreference == RDGQueuePreference::ePreferAsyncCompute)
    {
        switch (m_asyncComputeStatus)
        {
            case AsyncComputeStatus::eAvailable: result = RDGAsyncComputeEligibility::eEligible; break;
            case AsyncComputeStatus::eDisabled: result = RDGAsyncComputeEligibility::ePolicyDisabled; break;
            case AsyncComputeStatus::eComputeUnavailable: result = RDGAsyncComputeEligibility::eComputeUnavailable; break;
            case AsyncComputeStatus::eSharedGraphicsQueue: result = RDGAsyncComputeEligibility::eSharedGraphicsQueue; break;
            case AsyncComputeStatus::eDependenciesUnavailable:
                result = RDGAsyncComputeEligibility::eDependenciesUnavailable;
                break;
        }

        if (result == RDGAsyncComputeEligibility::eEligible
            && (!m_pRHIExecutor->GetQueueCopyCapabilities(RHICommandContextType::eAsyncCompute).compute
                || (node.type == RDGNodeType::eTransferPass
                    && !static_cast<const RDGTransferPass*>(node.pCompiledPass)->queueCapabilities.compute)))
        {
            result = RDGAsyncComputeEligibility::eUnsupportedCommands;
        }

        for (uint32_t i = 0; i < node.accessCount && result == RDGAsyncComputeEligibility::eEligible; ++i)
        {
            const RDGResourceManager::Allocation* allocation =
                graph.m_resourceManager.FindResourceByIdx(graph.m_accesses[node.accessOffset + i].resourceId);

            const RHIResource* resource =
                allocation->pTexture != nullptr ? static_cast<const RHIResource*>(allocation->pTexture) : allocation->pBuffer;

            if (!RHIQueueSupportsStages(m_pRHIExecutor->GetQueueCopyCapabilities(RHICommandContextType::eAsyncCompute),
                                        graph.m_accesses[node.accessOffset + i].pipelineStages))
            {
                result = RDGAsyncComputeEligibility::eUnsupportedCommands;
            }
            else if (IsViewportResource(resource))
            {
                result = RDGAsyncComputeEligibility::eViewportResource;
            }
            else if (allocation->hasInitialState)
            {
                // This import contract supplies external state/visibility but no protocol
                // for transferring ownership or synchronizing an additional native queue.
                result = RDGAsyncComputeEligibility::eExternalState;
            }
            else if (resource != nullptr && !resource->IsAsyncComputeAccessible())
            {
                result = RDGAsyncComputeEligibility::eResourceUnavailable;
            }
        }
    }

    return result;
}

bool RenderDevice::IsViewportResource(const RHIResource* resource) const
{
    bool found = false;

    if (resource != nullptr)
    {
        if (m_pMainViewport != nullptr)
        {
            found =
                resource == m_pMainViewport->GetColorBackBuffer() || resource == m_pMainViewport->GetDepthStencilBackBuffer();
        }

        for (RHIViewport* viewport : m_viewports)
        {
            found |= resource == viewport->GetColorBackBuffer() || resource == viewport->GetDepthStencilBackBuffer();
        }
    }

    return found;
}

void RenderDevice::EndFrame()
{
    if ((!(!m_frameActive || m_submissionBlocked)) && (!(m_pUploadQueue != nullptr && !m_pUploadQueue->Flush())))
    {
        const RHISubmissionResult result = GDynamicRHI->FlushAllGPUCommands();

        StampOutgoingFrameSerials();

        m_submissionBlocked |= result == RHISubmissionResult::eFatal;

        if (result == RHISubmissionResult::eSuccess)
        {
            GDynamicRHI->EndFrame();

            m_frameActive = false;
        }
    }
}

void RenderDevice::AcquireScheduledCmdLists(const RDGSchedule& schedule, HeapVector<RHICommandList*>& lists)
{
    ASSERT(lists.empty());

    lists.reserve(schedule.groups.size());

    for (const RDGSubmissionGroup& group : schedule.groups)
    {
        RHICommandList* list = nullptr;

        switch (group.queue)
        {
            case RHICommandContextType::eGraphics: list = m_graphicsCmdListPool.Acquire(); break;
            case RHICommandContextType::eAsyncCompute: list = m_computeCmdListPool.Acquire(); break;
            case RHICommandContextType::eTransfer: list = m_transferCmdListPool.Acquire(); break;
            default: ASSERT(false); break;
        }

        lists.push_back(list);
    }
}

void RenderDevice::ReleaseScheduledCmdLists(VectorView<RHICommandList*> lists)
{
    for (RHICommandList* list : lists)
    {
        if (list != nullptr)
        {
            switch (list->GetContext()->GetContextType())
            {
                case RHICommandContextType::eGraphics: m_graphicsCmdListPool.Release(list); break;
                case RHICommandContextType::eAsyncCompute: m_computeCmdListPool.Release(list); break;
                case RHICommandContextType::eTransfer: m_transferCmdListPool.Release(list); break;
                default: ASSERT(false); break;
            }
        }
    }
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

void RenderDevice::PipelineKey::AddAttachment(const RHIRenderTarget& target)
{
    Add(target.format);

    Add(target.numSamples);

    Add(int64_t(target.GetAspects()));
}

RenderDevice::PipelineKey RenderDevice::MakePipelineKey(RHIShader*                                             shader,
                                                        const RHIGfxPipelineStates*                            graphics,
                                                        const RHIRenderingLayout*                              layout,
                                                        const HashMap<uint32_t, RHIShaderSpecializationValue>& constants)
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

        key.Add(layout->numColorRenderTargets);

        key.Add(layout->hasDepthStencilRT);

        for (uint32_t i = 0; i < layout->numColorRenderTargets; ++i)
        {
            key.AddAttachment(layout->colorRenderTargets[i]);
        }

        if (layout->hasDepthStencilRT)
        {
            key.AddAttachment(layout->depthStencilRenderTarget);
        }

        SmallVector<std::pair<uint32_t, RHIShaderSpecializationValue>, 8> sorted;

        sorted.reserve(constants.size());

        for (const std::pair<const uint32_t, RHIShaderSpecializationValue>& item : constants)
        {
            sorted.emplace_back(item.first, item.second);
        }

        std::sort(sorted.begin(), sorted.end());

        key.Add(uint64_t(sorted.size()));

        for (const std::pair<uint32_t, RHIShaderSpecializationValue>& constant : sorted)
        {
            key.Add(constant.first);

            key.Add(constant.second.bits);

            key.Add(ToUnderlying(constant.second.type));
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

    return CreateInitializedBuffer(createInfo, dataSize, pData, false);
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

    return CreateInitializedBuffer(createInfo, dataSize, pData, false);
}

RHIBuffer* RenderDevice::CreateUniformBuffer(uint32_t dataSize, const uint8_t* pData, NameID bufferName)
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

    return CreateInitializedBuffer(createInfo, dataSize, pData, true);
}

RHIBuffer* RenderDevice::CreateStorageBuffer(uint32_t dataSize, const uint8_t* pData, NameID bufferName)
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

    return CreateInitializedBuffer(createInfo, dataSize, pData, true);
}

RHIBuffer* RenderDevice::CreateIndirectBuffer(uint32_t dataSize, const uint8_t* pData, NameID bufferName)
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

    return CreateInitializedBuffer(createInfo, dataSize, pData, true);
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

    RHISampler* sampler      = nullptr;

    if (m_samplerCache.contains(samplerHash))
    {
        sampler = m_samplerCache[samplerHash];
    }
    else
    {
        sampler = m_pRHIExecutor->CreateSampler(samplerInfo);

        if (sampler != nullptr)
        {
            m_samplerCache.emplace(samplerHash, sampler);
        }
    }

    return sampler;
}

RHITexture* RenderDevice::LoadTexture2D(const std::string& file, bool requireMipmap)
{
    return m_pTextureManager->LoadTexture2D(file, requireMipmap);
}

void RenderDevice::LoadSceneTextures(const sg::Scene* pScene, HeapVector<RHITexture*>& outTextures)
{
    m_pTextureManager->LoadSceneTextures(pScene, outTextures);
}

bool RenderDevice::LoadTextureEnv(const std::string& file, EnvTexture* pTexture, bool fallbackToBlack)
{
    const std::filesystem::path fullPath = std::filesystem::u8path(ZEN_TEXTURE_PATH) / std::filesystem::u8path(file);

    const std::u8string utf8             = fullPath.generic_u8string();

    return m_pTextureManager->LoadTextureEnv(std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size()), pTexture,
                                             fallbackToBlack);
}

void RenderDevice::LoadSceneEnvironment(const sg::Scene* scene, EnvTexture* environment)
{
    m_pTextureManager->LoadSceneEnvironment(scene, environment);
}

bool RenderDevice::ReleaseSceneTexture(RHITexture* texture)
{
    const bool released = m_pTextureManager != nullptr && m_pTextureManager->ReleaseSceneTexture(texture);

    return released;
}

void RenderDevice::ReleaseSceneEnvironment(EnvTexture* environment)
{
    if (m_pTextureManager != nullptr)
    {
        m_pTextureManager->ReleaseSceneEnvironment(environment);
    }
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
