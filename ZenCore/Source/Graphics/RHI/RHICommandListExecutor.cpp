#include "Graphics/RHI/RHICommandListExecutor.h"
#include "Utils/Errors.h"
#include <algorithm>
#include <exception>

namespace zen
{
RHISubmissionState::RHISubmissionState(VectorView<const RHICommandContextType> queues)
{
    for (RHICommandContextType queue : queues)
    {
        m_groups.push_back({{queue, 0}, false});
        m_failed |= size_t(queue) >= RHICompletionSet::kQueueCount;
    }
}

bool RHISubmissionState::Queue()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const bool result = !m_queued && !m_failed;
    if (result)
    {
        m_queued = true;
    }
    return result;
}

bool RHISubmissionState::IsQueued() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queued && !m_failed;
}

bool RHISubmissionState::Matches(uint32_t group, RHICommandContextType queue) const
{
    // Only the serial/accepted fields are mutable; queue and vector storage are immutable.
    return group < m_groups.size() && m_groups[group].submission.queue == queue;
}

size_t RHISubmissionState::GetGroupCount() const
{
    return m_groups.size();
}

bool RHISubmissionState::Accept(uint32_t group, RHISubmissionDependency submission)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const bool result = m_queued && !m_failed && Matches(group, submission.queue) &&
        submission.serial != RHISubmissionDependency::kLatestSubmitted && !m_groups[group].accepted;
    if (result)
    {
        m_groups[group].submission.serial = submission.serial;
        m_groups[group].accepted          = true;
    }
    return result;
}

void RHISubmissionState::Fail()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_failed = true;
}

bool RHISubmissionState::FinishSubmission()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    bool result = m_queued && !m_failed;
    for (const Group& group : m_groups)
    {
        result &= group.accepted;
    }
    m_submissionFinished = result;
    return result;
}

bool RHISubmissionState::IsSubmissionFinished() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_submissionFinished && !m_failed;
}

RHISubmissionPointStatus RHISubmissionState::Resolve(uint32_t group,
                                                     RHISubmissionDependency& submission) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    RHISubmissionPointStatus result = RHISubmissionPointStatus::eInvalid;
    if (group < m_groups.size())
    {
        if (m_failed)
        {
            result = RHISubmissionPointStatus::eFailed;
        }
        else if (m_groups[group].accepted)
        {
            submission = m_groups[group].submission;
            result     = RHISubmissionPointStatus::eAccepted;
        }
        else
        {
            result = RHISubmissionPointStatus::ePending;
        }
    }
    return result;
}

bool RHISubmissionPoint::IsValid() const
{
    return size_t(queue) < RHICompletionSet::kQueueCount &&
        (state ? serial == 0 && state->Matches(group, queue) :
                 serial != 0 && serial != RHISubmissionDependency::kLatestSubmitted &&
                 group == UINT32_MAX);
}

bool RHISubmissionPoint::operator==(const RHISubmissionPoint& other) const
{
    return queue == other.queue && serial == other.serial && state.Get() == other.state.Get() &&
        group == other.group;
}

RHISubmissionPointStatus RHISubmissionPoint::Resolve(RHISubmissionDependency& submission) const
{
    RHISubmissionPointStatus result = RHISubmissionPointStatus::eInvalid;
    if (IsValid())
    {
        if (state)
        {
            result = state->Resolve(group, submission);
        }
        else
        {
            submission = {queue, serial};
            result     = RHISubmissionPointStatus::eAccepted;
        }
    }
    return result;
}

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

const RHIBatchResult& RHISubmissionTicket::Wait() const
{
    VERIFY_EXPR(m_result.valid());
    if (m_result.valid() && !IsReady())
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
    m_submissionQueueCapabilities(backend->GetQueueCapabilities())
{
    for (uint32_t i = 0; i < RHICompletionSet::kQueueCount; ++i)
    {
        m_queueCapabilities.push_back(
            backend->GetQueueCopyCapabilities(static_cast<RHICommandContextType>(i)));
        m_queueProgress.push_back(MakeUnique<QueueProgress>());
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
    for (uint32_t i = 0; i < RHICompletionSet::kQueueCount; ++i)
    {
        const RHICommandContextType type = static_cast<RHICommandContextType>(i);
        m_queueProgress[i]->submitted.store(m_backend->GetLastSubmittedSerial(type),
                                            std::memory_order_release);
    }
    // A completion query may fail after work was accepted on another queue.
    // Capture every submitted serial before querying completion for any queue.
    for (uint32_t i = 0; i < RHICompletionSet::kQueueCount; ++i)
    {
        const RHICommandContextType type = static_cast<RHICommandContextType>(i);
        m_queueProgress[i]->completed.store(m_backend->QueryLastCompletedSerial(type),
                                            std::memory_order_release);
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
    const RHICompletionSet progress = GetCachedCompletedSerials();
    for (HeapVector<RefCountPtr<RHICommandBatch>>::iterator it = m_retired.begin();
         it != m_retired.end();)
    {
        const bool completed = (*it)->executionFinished &&
            !m_blocked.load(std::memory_order_acquire) &&
            (*it)->result.requiredSerials.IsCompleteAt(progress);
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

RHICompletionSet RHICommandListExecutor::GetCachedSubmittedSerials() const
{
    RHICompletionSet progress;
    for (size_t i = 0; i < RHICompletionSet::kQueueCount; ++i)
    {
        progress.serials[i] = m_queueProgress[i]->submitted.load(std::memory_order_acquire);
    }
    return progress;
}

RHICompletionSet RHICommandListExecutor::GetCachedCompletedSerials() const
{
    RHICompletionSet progress;
    for (size_t i = 0; i < RHICompletionSet::kQueueCount; ++i)
    {
        progress.serials[i] = m_queueProgress[i]->completed.load(std::memory_order_acquire);
    }
    return progress;
}

RHICommandListPtr RHICommandListExecutor::AcquireRecordingCommandList(RHICommandContextType queue)
{
    RHICommandListPtr result;
    {
        std::lock_guard<std::mutex> lock(m_recordingCommandListMutex);
        HeapVector<RHICommandListPtr>& lists = m_recordingCommandLists[size_t(queue)];
        if (!lists.empty())
        {
            result = std::move(lists.back());
            lists.pop_back();
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
    for (RHICommandBatch::Group& group : batch.groups)
    {
        const RHICommandContextType queue = group.commands->GetContext()->GetContextType();
        group.commands->ResetForReuse();
        std::lock_guard<std::mutex> lock(m_recordingCommandListMutex);
        HeapVector<RHICommandListPtr>& lists = m_recordingCommandLists[size_t(queue)];
        if (lists.size() < kMaxRecycledCommandLists)
        {
            lists.push_back(std::move(group.commands));
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

RHIQueueCapabilities RHICommandListExecutor::GetQueueCapabilities() const
{
    return m_submissionQueueCapabilities;
}

bool RHICommandListExecutor::PrepareSubmissionDependencies(
    IRHICommandContext* context,
    VectorView<const RHISubmissionDependency> dependencies)
{
    return GetRHIThread().Invoke(&DynamicRHI::PrepareSubmissionDependencies, m_backend, context,
                                 dependencies);
}

RHISubmissionTicket RHICommandListExecutor::SubmitFrame(
    RHICommandList& commands,
    RHIViewport* viewport,
    RefCountPtr<RHISubmissionState> state,
    VectorView<const RHISubmissionPoint> predecessors)
{
    RHISubmissionGroup group;
    group.commands     = &commands;
    group.predecessors = HeapVector<RHISubmissionPoint>(predecessors.data(), predecessors.size());
    return SubmitFrame(MakeVecView(group), viewport, std::move(state));
}

RHISubmissionTicket RHICommandListExecutor::SubmitFrame(VectorView<const RHISubmissionGroup> groups,
                                                        RHIViewport* viewport,
                                                        RefCountPtr<RHISubmissionState> state)
{
    return QueueBatch(groups, viewport, std::move(state), true);
}

RHIBatchResult RHICommandListExecutor::SubmitGroups(VectorView<const RHISubmissionGroup> groups,
                                                    RefCountPtr<RHISubmissionState> state)
{
    const RHISubmissionTicket ticket = QueueBatch(groups, nullptr, std::move(state), false);
    RHIBatchResult result;
    if (ticket.IsValid())
    {
        result = ticket.Wait();
    }
    else if (AreSubmissionsBlocked())
    {
        result.submission = RHISubmissionResult::eFatal;
    }
    return result;
}

bool RHICommandListExecutor::ValidateGroups(VectorView<const RHISubmissionGroup> groups,
                                            const RHISubmissionState& state) const
{
    bool valid = state.GetGroupCount() == groups.size();
    for (size_t i = 0; valid && i < groups.size(); ++i)
    {
        const RHISubmissionGroup& group = groups[i];
        valid = group.commands != nullptr && group.commands->GetContext() != nullptr;
        if (valid)
        {
            valid = state.Matches(uint32_t(i), group.commands->GetContext()->GetContextType());
        }
        for (size_t j = 0; valid && j < i; ++j)
        {
            valid = group.commands != groups[j].commands;
        }
        for (const RHISubmissionPoint& point : group.predecessors)
        {
            valid &= point.IsValid() &&
                (point.state.Get() == &state ? point.group < i :
                                               !point.state || point.state->IsQueued());
        }
    }
    return valid;
}

RHISubmissionTicket RHICommandListExecutor::QueueBatch(VectorView<const RHISubmissionGroup> groups,
                                                       RHIViewport* viewport,
                                                       RefCountPtr<RHISubmissionState> state,
                                                       bool endFrame)
{
    RHISubmissionTicket ticket;
    bool valid = std::this_thread::get_id() == m_renderThread && !AreSubmissionsBlocked();
    if (!state)
    {
        HeapVector<RHICommandContextType> queues;
        for (const RHISubmissionGroup& group : groups)
        {
            queues.push_back(group.commands != nullptr && group.commands->GetContext() != nullptr ?
                                 group.commands->GetContext()->GetContextType() :
                                 RHICommandContextType::eMax);
        }
        state = MakeRefCountPtr<RHISubmissionState>(queues);
    }
    valid = valid && ValidateGroups(groups, *state);
    if (valid && state->Queue())
    {
        RefCountPtr<RHICommandBatch> batch = MakeRefCountPtr<RHICommandBatch>();
        batch->submissionState             = state;
        batch->endFrame                    = endFrame;
        batch->viewport                    = viewport;
        batch->resources.Retain(viewport);
        if (viewport != nullptr)
        {
            batch->resources.Retain(viewport->GetColorBackBuffer());
            batch->resources.Retain(viewport->GetDepthStencilBackBuffer());
        }
        for (const RHISubmissionGroup& input : groups)
        {
            RHICommandBatch::Group& group = batch->groups.emplace_back();
            group.predecessors            = input.predecessors;
            group.commands                = input.commands->DetachCommands(
                AcquireRecordingCommandList(input.commands->GetContext()->GetContextType()));
        }
        batch->result.groups.resize(groups.size());
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

bool RHICommandListExecutor::ResolvePredecessors(RHICommandList& commands,
                                                 VectorView<const RHISubmissionPoint> points)
{
    bool ready = true;
    for (const RHISubmissionPoint& point : points)
    {
        RHISubmissionDependency resolved;
        ready = ready && point.Resolve(resolved) == RHISubmissionPointStatus::eAccepted;
        if (ready)
        {
            ready = resolved.serial <= m_backend->GetLastSubmittedSerial(resolved.queue);
            if (ready && resolved.queue != commands.GetContext()->GetContextType())
            {
                commands.AddSubmissionDependency(resolved);
            }
        }
    }
    return ready;
}

bool RHICommandListExecutor::ExecuteGroups(RHICommandBatch& batch)
{
    batch.result.submission = RHISubmissionResult::eSuccess;
    bool acceptedWork       = false;
    for (size_t i = 0;
         batch.result.submission == RHISubmissionResult::eSuccess && i < batch.groups.size(); ++i)
    {
        RHICommandBatch::Group& group    = batch.groups[i];
        RHICommandList* commands         = group.commands.get();
        RHISubmissionGroupResult& result = batch.result.groups[i];
        // Preserve an attempted group's fatal status if an existing backend exception escapes.
        result.submission                 = RHISubmissionResult::eFatal;
        const RHICommandContextType queue = commands->GetContext()->GetContextType();
        const uint64_t before             = m_backend->GetLastSubmittedSerial(queue);
        // Submit only this ready group. A later consumer cannot preflight a future serial.
        result.submission = ResolvePredecessors(*commands, group.predecessors) ?
            ExecuteBatch(MakeVecView(&commands, 1)) :
            RHISubmissionResult::eFatal;
        result.accepted   = {queue, m_backend->GetLastSubmittedSerial(queue)};
        if (result.submission == RHISubmissionResult::eSuccess)
        {
            if (!batch.submissionState->Accept(uint32_t(i), result.accepted))
            {
                result.submission = RHISubmissionResult::eFatal;
            }
        }
        else if (acceptedWork || result.accepted.serial > before)
        {
            result.submission = RHISubmissionResult::eFatal;
        }
        acceptedWork |= result.accepted.serial > before;
        batch.result.submission = result.submission;
    }
    return acceptedWork;
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
            beganExecution          = true;
            const bool acceptedWork = ExecuteGroups(*batch);
            if (result.submission == RHISubmissionResult::eSuccess && batch->viewport != nullptr)
            {
                batch->presentCommands = AcquirePresentCommandList();
                batch->viewport->PrepareForPresent(batch->presentCommands.get());
                RHICommandList* present = batch->presentCommands.get();
                const uint64_t before =
                    m_backend->GetLastSubmittedSerial(RHICommandContextType::eGraphics);
                result.submission = ExecuteBatch(MakeVecView(&present, 1));
                if (result.submission == RHISubmissionResult::eRejected &&
                    (acceptedWork ||
                     m_backend->GetLastSubmittedSerial(RHICommandContextType::eGraphics) > before))
                {
                    result.submission = RHISubmissionResult::eFatal;
                }
                if (result.submission == RHISubmissionResult::eSuccess)
                {
                    result.presented       = batch->viewport->Present();
                    result.needsRecreation = batch->viewport->NeedsRecreation();
                }
            }
            if (result.submission == RHISubmissionResult::eSuccess && batch->endFrame)
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
        batch->submissionState->Fail();
        if (result.error.empty())
        {
            result.error = result.submission == RHISubmissionResult::eRejected ?
                "Native submission rejected an asynchronously queued frame" :
                "Native submission failed after possible partial execution";
        }
        if (batch->endFrame || result.submission == RHISubmissionResult::eFatal)
        {
            m_blocked.store(true, std::memory_order_release);
        }
    }
    try
    {
        PublishProgress();
        if (AreSubmissionsBlocked() && result.submission == RHISubmissionResult::eSuccess)
        {
            result.submission = RHISubmissionResult::eFatal;
            result.error      = "Backend failed while querying RHI submission progress";
        }
    }
    catch (...)
    {
        result.submission = RHISubmissionResult::eFatal;
        result.error      = "Could not query RHI submission progress";
        m_blocked.store(true, std::memory_order_release);
    }
    result.requiredSerials.Extend(GetCachedSubmittedSerials());
    if (result.submission == RHISubmissionResult::eSuccess &&
        !batch->submissionState->FinishSubmission())
    {
        result.submission = RHISubmissionResult::eFatal;
        result.error      = "Frame submission did not accept every scheduled producer";
        m_blocked.store(true, std::memory_order_release);
    }
    if (result.submission != RHISubmissionResult::eSuccess)
    {
        batch->submissionState->Fail();
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

bool RHICommandListExecutor::WaitForCompletion(RHICommandContextType type,
                                               uint64_t serial,
                                               uint64_t timeoutNS)
{
    const bool completed =
        GetRHIThread().Invoke(&DynamicRHI::WaitForCompletion, m_backend, type, serial, timeoutNS);
    GetRHIThread().Invoke(&RHICommandListExecutor::PublishProgress, this);
    GetRHIThread().Invoke(&RHICommandListExecutor::CollectCompletedBatches, this, false);
    return completed;
}

uint64_t RHICommandListExecutor::GetLastSubmittedSerial(RHICommandContextType type) const
{
    return m_mode == RHIExecutionMode::eInline ?
        m_backend->GetLastSubmittedSerial(type) :
        m_queueProgress[static_cast<uint32_t>(type)]->submitted.load(std::memory_order_acquire);
}

uint64_t RHICommandListExecutor::QueryLastCompletedSerial(RHICommandContextType type)
{
    uint64_t completed =
        m_queueProgress[static_cast<uint32_t>(type)]->completed.load(std::memory_order_acquire);
    if (m_mode == RHIExecutionMode::eInline)
    {
        completed = m_backend->QueryLastCompletedSerial(type);
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
