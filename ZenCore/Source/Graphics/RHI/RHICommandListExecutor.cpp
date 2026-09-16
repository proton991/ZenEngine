#include "Graphics/RHI/RHICommandListExecutor.h"
#include "Utils/Errors.h"
#include <algorithm>
#include <exception>

namespace zen
{
RHISubmissionTicket::RHISubmissionTicket(std::shared_future<RHIBatchResult> result,
                                         RefCountPtr<RHIThreadEvent> completion) :
    m_result(std::move(result)), m_completion(std::move(completion))
{}

bool RHISubmissionTicket::IsValid() const
{
    return m_result.valid();
}

bool RHISubmissionTicket::IsReady() const
{
    return m_result.valid() &&
        m_result.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

RHIBatchResult RHISubmissionTicket::Wait() const
{
    VERIFY_EXPR(m_result.valid());
    if (m_result.valid())
    {
        m_completion->Wait();
    }
    return m_result.get();
}

RHICommandListExecutor::RHICommandListExecutor(DynamicRHI* backend, RHIExecutionMode mode) :
    m_backend(backend),
    m_mode(mode),
    m_renderThread(std::this_thread::get_id()),
    m_gpuInfo(backend->QueryGPUInfo()),
    m_apiType(backend->GetAPIType()),
    m_name(backend->GetName()),
    m_depthFormat(backend->GetSupportedDepthFormat()),
    m_sharedTransfer(backend->IsTransferQueueSharedWithGraphics()),
    m_queueCapabilities(3)
{
    for (uint32_t i = 0; i < 3; ++i)
    {
        m_queueCapabilities[i] =
            backend->GetQueueCopyCapabilities(static_cast<RHICommandContextType>(i));
    }
    GetRHIThread().Start(mode);
    GetRHIThread().Invoke(&RHICommandListExecutor::PublishProgress, this);
}

RHICommandListExecutor::~RHICommandListExecutor()
{
    if (!m_destroyed)
    {
        Destroy();
    }
    GetRHIThread().Invoke(&RHICommandListExecutor::DeleteBackend, this);
    GetRHIThread().Stop();
}

void RHICommandListExecutor::DeleteBackend()
{
    ZEN_DELETE(m_backend);
    m_backend = nullptr;
}

void RHICommandListExecutor::NotifyResourceDestroyed(uint64_t resourceId)
{
    std::lock_guard<std::mutex> lock(m_destroyedResourceMutex);
    m_destroyedResourceIds.push_back(resourceId);
}

void RHICommandListExecutor::DrainDestroyedResourceIds(HeapVector<uint64_t>& resourceIds)
{
    ASSERT(std::this_thread::get_id() == m_renderThread);
    resourceIds.clear();
    std::lock_guard<std::mutex> lock(m_destroyedResourceMutex);
    std::swap(resourceIds, m_destroyedResourceIds);
}

void RHICommandListExecutor::Destroy()
{
    if (!m_destroyed)
    {
        GetRHIThread().Invoke(&RHICommandListExecutor::ExecuteDestroy, this);
        m_destroyed = true;
    }
}

void RHICommandListExecutor::ExecuteDestroy()
{
    ExecuteWaitIdle();
    CollectCompletedBatches(true);
    {
        std::lock_guard<std::mutex> lock(m_recordingCommandListMutex);
        m_recordingCommandLists.clear();
    }
    m_presentCommandLists.clear();
    m_backend->Destroy();
}

void RHICommandListExecutor::PublishProgress()
{
    GetRHIThread().CheckOwnership();
    for (uint32_t i = 0; i < 3; ++i)
    {
        const RHICommandContextType type = static_cast<RHICommandContextType>(i);
        m_submitted[i].store(m_backend->GetLastSubmittedSerial(type), std::memory_order_release);
        m_completed[i].store(m_backend->GetLastCompletedSerial(type), std::memory_order_release);
    }
    PublishSubmissionStatus();
}

void RHICommandListExecutor::PublishSubmissionStatus()
{
    GetRHIThread().CheckOwnership();
    if (m_backend->AreSubmissionsBlocked())
    {
        m_blocked.store(true, std::memory_order_release);
    }
}

void RHICommandListExecutor::CollectCompletedBatches(bool force)
{
    for (HeapVector<RefCountPtr<RHICommandBatch>>::iterator it = m_retired.begin();
         it != m_retired.end();)
    {
        bool completed = (*it)->executionFinished && !m_blocked.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < 3; ++i)
        {
            completed = completed &&
                m_completed[i].load(std::memory_order_acquire) >= (*it)->result.serials[i];
        }
        if (force || completed)
        {
            if (!force)
            {
                RecycleCommandLists(**it);
            }
            it = m_retired.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

RHICommandListPtr RHICommandListExecutor::AcquireRecordingCommandList()
{
    RHICommandListPtr result;
    {
        std::lock_guard<std::mutex> lock(m_recordingCommandListMutex);
        if (!m_recordingCommandLists.empty())
        {
            result = std::move(m_recordingCommandLists.back());
            m_recordingCommandLists.pop_back();
        }
    }
    return result;
}

RHICommandListPtr RHICommandListExecutor::AcquirePresentCommandList()
{
    GetRHIThread().CheckOwnership();
    RHICommandListPtr result;
    if (!m_presentCommandLists.empty())
    {
        result = std::move(m_presentCommandLists.back());
        m_presentCommandLists.pop_back();
    }
    else
    {
        result.reset(
            RHICommandList::Create(m_backend->GetCommandContext(RHICommandContextType::eGraphics)));
    }
    return result;
}

void RHICommandListExecutor::RecycleCommandLists(RHICommandBatch& batch)
{
    GetRHIThread().CheckOwnership();
    batch.commands->ResetForReuse();
    {
        std::lock_guard<std::mutex> lock(m_recordingCommandListMutex);
        if (m_recordingCommandLists.size() < kMaxRecycledCommandLists)
        {
            m_recordingCommandLists.push_back(std::move(batch.commands));
        }
    }
    if (batch.presentCommands != nullptr)
    {
        batch.presentCommands->Reset();
        if (m_presentCommandLists.size() < kMaxRecycledCommandLists)
        {
            m_presentCommandLists.push_back(std::move(batch.presentCommands));
        }
    }
}

RHISubmissionResult RHICommandListExecutor::ExecuteBatch(VectorView<RHICommandList*> lists)
{
    GetRHIThread().CheckOwnership();
    RHISubmissionResult result = RHISubmissionResult::eFatal;
    PublishSubmissionStatus();
    if (!m_blocked.load(std::memory_order_acquire))
    {
        HeapVector<RHIPlatformCommandList*> platformLists;
        m_backend->FinalizeCommandLists(lists, platformLists);
        m_backend->SubmitPlatformCommandLists(platformLists);
        result = m_backend->FlushAllGPUCommands();
        if (result == RHISubmissionResult::eFatal)
        {
            m_blocked.store(true, std::memory_order_release);
        }
    }
    PublishProgress();
    if (AreSubmissionsBlocked())
    {
        result = RHISubmissionResult::eFatal;
    }
    CollectCompletedBatches();
    return result;
}

RHISubmissionResult RHICommandListExecutor::SubmitBatch(VectorView<RHICommandList*> lists)
{
    return GetRHIThread().Invoke(&RHICommandListExecutor::ExecuteBatch, this, lists);
}

RHISubmissionTicket RHICommandListExecutor::SubmitFrame(RHICommandList& commands,
                                                        RHIViewport* viewport)
{
    if (std::this_thread::get_id() != m_renderThread)
    {
        throw std::logic_error("Frame batches must be recorded on the RenderCore thread");
    }
    RHISubmissionTicket ticket;
    if (!AreSubmissionsBlocked())
    {
        RefCountPtr<RHICommandBatch> batch = MakeRefCountPtr<RHICommandBatch>();
        batch->viewport                    = viewport;
        batch->resources.Retain(viewport);
        if (viewport != nullptr)
        {
            batch->resources.Retain(viewport->GetColorBackBuffer());
            batch->resources.Retain(viewport->GetDepthStencilBackBuffer());
        }
        batch->commands = commands.DetachCommands(AcquireRecordingCommandList());
        batch->queuedAt = std::chrono::steady_clock::now();
        ticket =
            RHISubmissionTicket(batch->completion.get_future().share(), batch->completionEvent);
        ++m_submittedBatches;
        const uint64_t pendingCount = m_pendingBatches.fetch_add(1) + 1;
        m_peakPendingBatches.store(std::max(m_peakPendingBatches.load(), pendingCount));
        GetRHIThread().Dispatch(
            std::bind_front(&RHICommandListExecutor::ExecuteFrame, this, batch));
    }
    return ticket;
}

void RHICommandListExecutor::ExecuteFrame(const RefCountPtr<RHICommandBatch>& batch)
{
    GetRHIThread().CheckOwnership();
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    const RHIFrameNumber frameNumber                  = GRHIFrameState.GetFrameNumber();
    const RHIFrameSlot frameSlot                      = GRHIFrameState.GetFrameSlot();
    RHIBatchResult& result                            = batch->result;
    result.queueWaitUs = std::chrono::duration<double, std::micro>(start - batch->queuedAt).count();
    bool beganExecution = false;
    try
    {
        if (!AreSubmissionsBlocked())
        {
            // Reserve retirement ownership before native execution can fail.
            m_retired.push_back(batch);
            beganExecution           = true;
            RHICommandList* commands = batch->commands.get();
            result.submission        = ExecuteBatch(MakeVecView(&commands, 1));
            if (result.submission == RHISubmissionResult::eSuccess && batch->viewport != nullptr)
            {
                batch->presentCommands = AcquirePresentCommandList();
                batch->viewport->PrepareForPresent(batch->presentCommands.get());
                RHICommandList* present = batch->presentCommands.get();
                result.submission       = ExecuteBatch(MakeVecView(&present, 1));
                if (result.submission == RHISubmissionResult::eSuccess)
                {
                    result.presented       = batch->viewport->Present();
                    result.needsRecreation = batch->viewport->NeedsRecreation();
                }
            }
            if (result.submission == RHISubmissionResult::eSuccess)
            {
                m_backend->EndFrame();
            }
        }
        else
        {
            result.submission = RHISubmissionResult::eFatal;
            result.error      = "An earlier RHI batch failed";
        }
    }
    catch (const std::exception& error)
    {
        result.submission = RHISubmissionResult::eFatal;
        result.error      = error.what();
    }
    catch (...)
    {
        result.submission = RHISubmissionResult::eFatal;
        result.error      = "Unknown exception during RHI frame execution";
    }
    // Once accepted asynchronously, rejection invalidates dependent scheduled state.
    // Never execute later batches against state that did not reach the GPU.
    if (result.submission != RHISubmissionResult::eSuccess)
    {
        if (result.error.empty())
        {
            result.error = result.submission == RHISubmissionResult::eRejected ?
                "Native submission rejected an asynchronously queued frame" :
                "Native submission failed after possible partial execution";
        }
        m_blocked.store(true, std::memory_order_release);
    }
    try
    {
        PublishProgress();
        if (AreSubmissionsBlocked() && result.submission == RHISubmissionResult::eSuccess)
        {
            result.submission = RHISubmissionResult::eFatal;
            result.error      = "Backend failed while querying RHI submission progress";
        }
        for (uint32_t i = 0; i < 3; ++i)
        {
            result.serials[i] = m_submitted[i].load(std::memory_order_acquire);
        }
    }
    catch (...)
    {
        result.submission = RHISubmissionResult::eFatal;
        result.error      = "Could not query RHI submission progress";
        m_blocked.store(true, std::memory_order_release);
    }
    if (result.submission != RHISubmissionResult::eSuccess)
    {
        LOGE("RHI frame {} (slot {}) failed: {}", ToValue(frameNumber), ToIndex(frameSlot),
             result.error);
    }
    result.executionCPUUs =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
    m_executionCPUUs.fetch_add(static_cast<uint64_t>(result.executionCPUUs));
    m_queueWaitUs.fetch_add(static_cast<uint64_t>(result.queueWaitUs));
    ++m_completedBatches;
    --m_pendingBatches;
    batch->executionFinished = true;
    batch->completion.set_value(result);
    batch->completionEvent->Signal();
    if (beganExecution)
    {
        CollectCompletedBatches();
    }
}

void RHICommandListExecutor::ExecuteBeginFrame()
{
    try
    {
        if (!AreSubmissionsBlocked())
        {
            m_backend->BeginFrame();
            PublishProgress();
            CollectCompletedBatches();
        }
    }
    catch (...)
    {
        m_blocked.store(true, std::memory_order_release);
    }
}

void RHICommandListExecutor::BeginFrame()
{
    GetRHIThread().Dispatch(std::bind_front(&RHICommandListExecutor::ExecuteBeginFrame, this));
}

void RHICommandListExecutor::EndFrame()
{
    GetRHIThread().Invoke(&DynamicRHI::EndFrame, m_backend);
}

RHISubmissionResult RHICommandListExecutor::FlushAllGPUCommands()
{
    return GetRHIThread().Invoke(&RHICommandListExecutor::ExecuteBatch, this,
                                 VectorView<RHICommandList*>());
}

void RHICommandListExecutor::ExecuteWaitIdle()
{
    m_backend->WaitDeviceIdle();
    PublishProgress();
    CollectCompletedBatches();
}

void RHICommandListExecutor::WaitDeviceIdle()
{
    GetRHIThread().Invoke(&RHICommandListExecutor::ExecuteWaitIdle, this);
}

bool RHICommandListExecutor::WaitForSubmission(RHICommandContextType type,
                                               uint64_t serial,
                                               uint64_t timeoutNS)
{
    const bool completed =
        GetRHIThread().Invoke(&DynamicRHI::WaitForSubmission, m_backend, type, serial, timeoutNS);
    GetRHIThread().Invoke(&RHICommandListExecutor::PublishProgress, this);
    GetRHIThread().Invoke(&RHICommandListExecutor::CollectCompletedBatches, this, false);
    return completed;
}

uint64_t RHICommandListExecutor::GetLastSubmittedSerial(RHICommandContextType type) const
{
    return m_mode == RHIExecutionMode::eInline ?
        m_backend->GetLastSubmittedSerial(type) :
        m_submitted[static_cast<uint32_t>(type)].load(std::memory_order_acquire);
}

uint64_t RHICommandListExecutor::GetLastCompletedSerial(RHICommandContextType type)
{
    uint64_t completed = m_completed[static_cast<uint32_t>(type)].load(std::memory_order_acquire);
    if (m_mode == RHIExecutionMode::eInline)
    {
        completed = m_backend->GetLastCompletedSerial(type);
        PublishSubmissionStatus();
    }
    return completed;
}

void RHICommandListExecutor::RefreshGPUProgress()
{
    try
    {
        PublishProgress();
        CollectCompletedBatches();
        // Releasing command arenas can release the last recording of a bindless epoch.
        m_backend->CollectRetiredBindlessResources();
        PublishSubmissionStatus();
    }
    catch (...)
    {
        m_blocked.store(true, std::memory_order_release);
        throw;
    }
}

void RHICommandListExecutor::ExecutePollGPUProgress()
{
    try
    {
        RefreshGPUProgress();
    }
    catch (const std::exception& error)
    {
        LOGE("Could not poll RHI GPU progress: {}", error.what());
    }
    catch (...)
    {
        LOGE("Could not poll RHI GPU progress: unknown exception");
    }
    m_progressPollPending.store(false, std::memory_order_release);
}

void RHICommandListExecutor::PollGPUProgress()
{
    bool pending = false;
    if (m_progressPollPending.compare_exchange_strong(pending, true, std::memory_order_acq_rel))
    {
        try
        {
            if (!GetRHIThread().TryDispatch(
                    std::bind_front(&RHICommandListExecutor::ExecutePollGPUProgress, this)))
            {
                m_progressPollPending.store(false, std::memory_order_release);
            }
        }
        catch (...)
        {
            m_progressPollPending.store(false, std::memory_order_release);
            throw;
        }
    }
}

void RHICommandListExecutor::FlushRHIThread()
{
    // The ordered invocation fences CPU work and samples GPU progress without waiting
    // for unfinished GPU submissions or changing the CPU-only flush contract.
    GetRHIThread().Invoke(&RHICommandListExecutor::RefreshGPUProgress, this);
}

bool RHICommandListExecutor::AreSubmissionsBlocked() const
{
    return m_blocked.load(std::memory_order_acquire);
}

RHIThreadMetrics RHICommandListExecutor::GetThreadMetrics() const
{
    return {m_submittedBatches.load(), m_completedBatches.load(), m_executionCPUUs.load(),
            m_queueWaitUs.load(),      m_pendingBatches.load(),   m_peakPendingBatches.load()};
}

RHIAPIType RHICommandListExecutor::GetAPIType()
{
    return m_apiType;
}

NameID RHICommandListExecutor::GetName()
{
    return m_name;
}

DataFormat RHICommandListExecutor::GetSupportedDepthFormat()
{
    return m_depthFormat;
}

bool RHICommandListExecutor::IsTransferQueueSharedWithGraphics() const
{
    return m_mode == RHIExecutionMode::eInline ? m_backend->IsTransferQueueSharedWithGraphics() :
                                                 m_sharedTransfer;
}

const RHIGPUInfo& RHICommandListExecutor::QueryGPUInfo() const
{
    return m_mode == RHIExecutionMode::eInline ? m_backend->QueryGPUInfo() : m_gpuInfo;
}

RHIQueueCopyCapabilities RHICommandListExecutor::GetQueueCopyCapabilities(
    RHICommandContextType type) const
{
    return m_mode == RHIExecutionMode::eInline ? m_backend->GetQueueCopyCapabilities(type) :
                                                 m_queueCapabilities[static_cast<uint32_t>(type)];
}

void RHICommandListExecutor::Init()
{
    GetRHIThread().Invoke(&DynamicRHI::Init, m_backend);
}

void RHICommandListExecutor::DestroyViewport(RHIViewport* viewport)
{
    GetRHIThread().Invoke(&DynamicRHI::DestroyViewport, m_backend, viewport);
}

RHIViewport* RHICommandListExecutor::CreateViewport(void* window,
                                                    uint32_t width,
                                                    uint32_t height,
                                                    bool vsync)
{
    ASSERT(std::this_thread::get_id() == m_renderThread);
    return m_backend->CreateViewport(window, width, height, vsync);
}

RHIShader* RHICommandListExecutor::CreateShader(const RHIShaderCreateInfo& info)
{
    return GetRHIThread().Invoke(&DynamicRHI::CreateShader, m_backend, std::cref(info));
}

void RHICommandListExecutor::DestroyShader(RHIShader* shader)
{
    GetRHIThread().Invoke(&DynamicRHI::DestroyShader, m_backend, shader);
}

RHIPipeline* RHICommandListExecutor::CreatePipeline(const RHIComputePipelineCreateInfo& info)
{
    return GetRHIThread().Invoke([&] { return m_backend->CreatePipeline(info); });
}

RHIPipeline* RHICommandListExecutor::CreatePipeline(const RHIGfxPipelineCreateInfo& info)
{
    return GetRHIThread().Invoke([&] { return m_backend->CreatePipeline(info); });
}

void RHICommandListExecutor::DestroyPipeline(RHIPipeline* pipeline)
{
    GetRHIThread().Invoke(&DynamicRHI::DestroyPipeline, m_backend, pipeline);
}

RHISampler* RHICommandListExecutor::CreateSampler(const RHISamplerCreateInfo& info)
{
    return GetRHIThread().Invoke(&DynamicRHI::CreateSampler, m_backend, std::cref(info));
}

void RHICommandListExecutor::DestroySampler(RHISampler* sampler)
{
    GetRHIThread().Invoke(&DynamicRHI::DestroySampler, m_backend, sampler);
}

RHIBindlessHandle RHICommandListExecutor::RegisterBindlessResource(RHIResource* resource,
                                                                   uint32_t slot)
{
    return GetRHIThread().Invoke(&DynamicRHI::RegisterBindlessResource, m_backend, resource, slot);
}

bool RHICommandListExecutor::UnregisterBindlessResource(RHIBindlessHandle handle)
{
    return GetRHIThread().Invoke(&DynamicRHI::UnregisterBindlessResource, m_backend, handle);
}

bool RHICommandListExecutor::IsBindlessResourceRegistered(RHIBindlessHandle handle)
{
    return GetRHIThread().Invoke(&DynamicRHI::IsBindlessResourceRegistered, m_backend, handle);
}

void RHICommandListExecutor::CollectRetiredBindlessResources()
{
    GetRHIThread().Invoke(&RHICommandListExecutor::RefreshGPUProgress, this);
}

RHITexture* RHICommandListExecutor::CreateTexture(const RHITextureCreateInfo& info)
{
    return GetRHIThread().Invoke(&DynamicRHI::CreateTexture, m_backend, std::cref(info));
}

RHITextureView* RHICommandListExecutor::CreateTextureView(RHITexture* texture,
                                                          const RHITextureViewCreateInfo& info)
{
    return GetRHIThread().Invoke(&DynamicRHI::CreateTextureView, m_backend, texture,
                                 std::cref(info));
}

void RHICommandListExecutor::DestroyTexture(RHITexture* texture)
{
    GetRHIThread().Invoke(&DynamicRHI::DestroyTexture, m_backend, texture);
}

RHIBuffer* RHICommandListExecutor::CreateBuffer(const RHIBufferCreateInfo& info)
{
    return GetRHIThread().Invoke(&DynamicRHI::CreateBuffer, m_backend, std::cref(info));
}

void RHICommandListExecutor::DestroyBuffer(RHIBuffer* buffer)
{
    GetRHIThread().Invoke(&DynamicRHI::DestroyBuffer, m_backend, buffer);
}

IRHICommandContext* RHICommandListExecutor::GetCommandContext(RHICommandContextType type)
{
    return GetRHIThread().Invoke(&DynamicRHI::GetCommandContext, m_backend, type);
}

IRHICommandContext* RHICommandListExecutor::GetTransferCommandContext()
{
    return GetRHIThread().Invoke(&DynamicRHI::GetTransferCommandContext, m_backend);
}

void RHICommandListExecutor::FinalizeCommandLists(VectorView<RHICommandList*> lists,
                                                  HeapVector<RHIPlatformCommandList*>& output)
{
    GetRHIThread().Invoke(&DynamicRHI::FinalizeCommandLists, m_backend, lists, std::ref(output));
}

void RHICommandListExecutor::SubmitPlatformCommandLists(VectorView<RHIPlatformCommandList*> lists)
{
    GetRHIThread().Invoke(&DynamicRHI::SubmitPlatformCommandLists, m_backend, lists);
}

RHITextureCopyCapabilities RHICommandListExecutor::GetTextureCopyCapabilities(
    DataFormat format) const
{
    return GetRHIThread().Invoke(&DynamicRHI::GetTextureCopyCapabilities, m_backend, format);
}
} // namespace zen
