#include "Graphics/RHI/RHICommandListExecutor.h"
#include "Utils/Errors.h"
#include <algorithm>

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

    const bool result = m_queued && !m_failed && Matches(group, submission.queue)
                     && submission.serial != RHISubmissionDependency::kLatestSubmitted && !m_groups[group].accepted;

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

RHISubmissionPointStatus RHISubmissionState::Resolve(uint32_t group, RHISubmissionDependency& submission) const
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
    return size_t(queue) < RHICompletionSet::kQueueCount
        && (state ? serial == 0 && state->Matches(group, queue)
                  : serial != 0 && serial != RHISubmissionDependency::kLatestSubmitted && group == UINT32_MAX);
}

bool RHISubmissionPoint::operator==(const RHISubmissionPoint& other) const
{
    return queue == other.queue && serial == other.serial && state.Get() == other.state.Get() && group == other.group;
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

RHISubmissionTicket::RHISubmissionTicket(std::shared_future<RHIBatchResult> result, RefCountPtr<RHIThreadEvent> completion) :
    m_result(std::move(result)), m_completion(std::move(completion))
{}

bool RHISubmissionTicket::IsValid() const
{
    return m_result.valid();
}

bool RHISubmissionTicket::IsReady() const
{
    return m_result.valid() && m_result.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
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
        m_queueCapabilities.push_back(backend->GetQueueCopyCapabilities(static_cast<RHICommandContextType>(i)));

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

    DeleteBackend();
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
        GetRHIThread().Stop(std::bind_front(&RHICommandListExecutor::ExecuteDestroy, this));

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

        m_queueProgress[i]->submitted.store(m_backend->GetLastSubmittedSerial(type), std::memory_order_release);
    }

    // A completion query may fail after work was accepted on another queue.
    // Capture every submitted serial before querying completion for any queue.
    for (uint32_t i = 0; i < RHICompletionSet::kQueueCount; ++i)
    {
        const RHICommandContextType type = static_cast<RHICommandContextType>(i);

        m_queueProgress[i]->completed.store(m_backend->QueryLastCompletedSerial(type), std::memory_order_release);
    }

    PublishSubmissionStatus();
}

void RHICommandListExecutor::PublishSubmissionStatus()
{
    GetRHIThread().CheckOwnership();

    if (m_backend->AreSubmissionsBlocked())
    {
        PublishTerminalError(m_backend->GetTerminalError());

        if (m_backend->HasDeviceLoss())
        {
            GetRHIThread().Fail(MakeRHIError(RHIErrorCode::eDeviceLost, "RHI device loss observed", __FILE__, __LINE__));
        }
    }
}

void RHICommandListExecutor::PublishTerminalError(RHIError error)
{
    if (!error.IsFailure())
    {
        error = MakeRHIError(RHIErrorCode::eBackendFailure, "RHI operation failed", __FILE__, __LINE__);
    }

    bool first = false;
    {
        std::lock_guard<std::mutex> lock(m_errorMutex);

        first = !m_terminalError.IsFailure();

        if (first)
        {
            m_terminalError = error;
        }
    }

    m_blocked.store(true, std::memory_order_release);

    GetRHIThread().Fail(error);

    if (first)
    {
        const RHICompletionSet submitted = GetCachedSubmittedSerials();

        LOGE(
            "RHI stopped at frame {} (slot {}): {} (code {}, native {}, {}:{}, resource {}); submitted graphics={}, compute={}, transfer={}",
            ToValue(GRHIFrameState.GetFrameNumber()), ToIndex(GRHIFrameState.GetFrameSlot()),
            error.operation != nullptr ? error.operation : "unknown operation", static_cast<uint32_t>(error.code),
            error.nativeCode, error.source != nullptr ? error.source : "unknown source", error.line, error.resourceId,
            submitted.serials[static_cast<uint32_t>(RHICommandContextType::eGraphics)],
            submitted.serials[static_cast<uint32_t>(RHICommandContextType::eAsyncCompute)],
            submitted.serials[static_cast<uint32_t>(RHICommandContextType::eTransfer)]);
    }
}

RHIError RHICommandListExecutor::GetTerminalError() const
{
    std::lock_guard<std::mutex> lock(m_errorMutex);

    return m_terminalError.IsFailure() ? m_terminalError : GetRHIThread().GetFailure();
}

void RHICommandListExecutor::CollectCompletedBatches(bool force)
{
    const RHICompletionSet progress = GetCachedCompletedSerials();

    for (HeapVector<RefCountPtr<RHICommandBatch>>::iterator it = m_retired.begin(); it != m_retired.end();)
    {
        const bool completed = (*it)->executionFinished && !m_blocked.load(std::memory_order_acquire)
                            && (*it)->result.requiredSerials.IsCompleteAt(progress);

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
        result.reset(RHICommandList::Create(m_backend->GetCommandContext(RHICommandContextType::eGraphics)));
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

    m_executionError           = {};

    PublishSubmissionStatus();

    if (!m_blocked.load(std::memory_order_acquire))
    {
        HeapVector<RHIPlatformCommandList*> platformLists;

        const RHIStatus finalized = m_backend->FinalizeCommandLists(lists, platformLists);

        if (finalized)
        {
            m_backend->SubmitPlatformCommandLists(platformLists);

            result = m_backend->FlushAllGPUCommands();

            if (result != RHISubmissionResult::eSuccess)
            {
                m_executionError = m_backend->GetLastSubmissionError();
            }
        }
        else
        {
            m_executionError = finalized.error;

            result           = finalized.error.code == RHIErrorCode::eDeviceLost ? RHISubmissionResult::eFatal
                                                                                 : RHISubmissionResult::eRejected;
        }

        if (result == RHISubmissionResult::eFatal)
        {
            PublishTerminalError(m_executionError);
        }
    }

    PublishProgress();

    if (AreSubmissionsBlocked())
    {
        result = RHISubmissionResult::eFatal;

        if (!m_executionError.IsFailure())
        {
            m_executionError = GetTerminalError();
        }
    }

    if (result != RHISubmissionResult::eSuccess && !m_executionError.IsFailure())
    {
        m_executionError = MakeRHIError(RHIErrorCode::eBackendFailure, "RHI submission", __FILE__, __LINE__);
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

bool RHICommandListExecutor::PrepareSubmissionDependencies(IRHICommandContext*                       context,
                                                           VectorView<const RHISubmissionDependency> dependencies)
{
    return GetRHIThread().Invoke(&DynamicRHI::PrepareSubmissionDependencies, m_backend, context, dependencies);
}

RHISubmissionTicket RHICommandListExecutor::SubmitFrame(RHICommandList&                      commands,
                                                        RHIViewport*                         viewport,
                                                        RefCountPtr<RHISubmissionState>      state,
                                                        VectorView<const RHISubmissionPoint> predecessors)
{
    RHISubmissionGroup group;

    group.commands     = &commands;

    group.predecessors = HeapVector<RHISubmissionPoint>(predecessors.data(), predecessors.size());

    return SubmitFrame(MakeVecView(group), viewport, std::move(state));
}

RHISubmissionTicket RHICommandListExecutor::SubmitFrame(VectorView<const RHISubmissionGroup> groups,
                                                        RHIViewport*                         viewport,
                                                        RefCountPtr<RHISubmissionState>      state)
{
    return QueueBatch(groups, viewport, std::move(state), true);
}

RHIBatchResult RHICommandListExecutor::SubmitGroups(VectorView<const RHISubmissionGroup> groups,
                                                    RefCountPtr<RHISubmissionState>      state,
                                                    RHIViewport*                         viewport)
{
    const RHISubmissionTicket ticket = QueueBatch(groups, viewport, std::move(state), false);

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

bool RHICommandListExecutor::ValidateGroups(VectorView<const RHISubmissionGroup> groups, const RHISubmissionState& state) const
{
    bool valid = state.GetGroupCount() == groups.size();

    for (size_t i = 0; valid && i < groups.size(); ++i)
    {
        const RHISubmissionGroup& group = groups[i];

        valid                           = group.commands != nullptr && group.commands->GetContext() != nullptr;

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
            valid &=
                point.IsValid() && (point.state.Get() == &state ? point.group < i : !point.state || point.state->IsQueued());
        }
    }

    return valid;
}

RHISubmissionTicket RHICommandListExecutor::QueueBatch(VectorView<const RHISubmissionGroup> groups,
                                                       RHIViewport*                         viewport,
                                                       RefCountPtr<RHISubmissionState>      state,
                                                       bool                                 endFrame)
{
    RHISubmissionTicket ticket;

    bool valid = std::this_thread::get_id() == m_renderThread && !AreSubmissionsBlocked();

    if (!state)
    {
        HeapVector<RHICommandContextType> queues;

        for (const RHISubmissionGroup& group : groups)
        {
            queues.push_back(group.commands != nullptr && group.commands->GetContext() != nullptr
                                 ? group.commands->GetContext()->GetContextType()
                                 : RHICommandContextType::eMax);
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

            group.commands =
                input.commands->DetachCommands(AcquireRecordingCommandList(input.commands->GetContext()->GetContextType()));
        }

        batch->result.groups.resize(groups.size());

        batch->queuedAt = std::chrono::steady_clock::now();

        ticket          = RHISubmissionTicket(batch->completion.get_future().share(), batch->completionEvent);

        ++m_submittedBatches;

        const uint64_t pendingCount = m_pendingBatches.fetch_add(1) + 1;

        m_peakPendingBatches.store(std::max(m_peakPendingBatches.load(), pendingCount));

        GetRHIThread().Dispatch(std::bind_front(&RHICommandListExecutor::ExecuteFrame, this, batch),
                                std::bind_front(&RHICommandListExecutor::CancelBatch, this, batch));
    }

    return ticket;
}

bool RHICommandListExecutor::ResolvePredecessors(RHICommandList& commands, VectorView<const RHISubmissionPoint> points)
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

uint64_t RHICommandListExecutor::GetAcceptedSerial(const RHICommandList& commands,
                                                   RHICommandContextType queue,
                                                   uint64_t              before) const
{
    // The group's own context stays exact when the presentation copy shares its native
    // submission. A context without newly accepted work falls back to the queue's serial.
    const uint64_t contextSerial = commands.GetContext()->RHIGetLastSubmittedSerial();

    const uint64_t serial        = contextSerial > before ? contextSerial : m_backend->GetLastSubmittedSerial(queue);

    return serial;
}

bool RHICommandListExecutor::ExecuteGroups(RHICommandBatch& batch)
{
    batch.result.submission = RHISubmissionResult::eSuccess;

    bool acceptedWork       = false;

    for (size_t i = 0; batch.result.submission == RHISubmissionResult::eSuccess && i < batch.groups.size(); ++i)
    {
        RHICommandBatch::Group& group    = batch.groups[i];

        RHICommandList* commands         = group.commands.get();

        RHISubmissionGroupResult& result = batch.result.groups[i];

        // Initialize attempted work conservatively until native acceptance is known.
        result.submission                 = RHISubmissionResult::eFatal;

        const RHICommandContextType queue = commands->GetContext()->GetContextType();

        const uint64_t before             = m_backend->GetLastSubmittedSerial(queue);

        // Submit only this ready group. A later consumer cannot preflight a future serial.
        bool ready = ResolvePredecessors(*commands, group.predecessors);

        // An unresolved predecessor is fatal. Missing presentation storage submits nothing
        // and stays retryable unless earlier work in this batch was already accepted.
        RHISubmissionResult unsubmitted = RHISubmissionResult::eFatal;

        RHICommandList* lists[]         = {commands, nullptr};

        size_t listCount                = 1;

        // The frame's last graphics group carries the presentation copy in the same native
        // submission. The copy keeps its own command buffer, so only the copy waits for the
        // acquired image.
        if (ready && batch.viewport != nullptr && i + 1 == batch.groups.size() && queue == RHICommandContextType::eGraphics)
        {
            batch.presentCommands = AcquirePresentCommandList();

            ready                 = batch.presentCommands != nullptr;

            if (ready)
            {
                const RHIStatus prepared = batch.viewport->PrepareForPresentChecked(batch.presentCommands.get());

                ready                    = static_cast<bool>(prepared);

                if (!ready)
                {
                    batch.result.cause = prepared.error;
                }

                lists[1]  = batch.presentCommands.get();

                listCount = 2;
            }
            else
            {
                unsubmitted = RHISubmissionResult::eRejected;
            }
        }

        result.submission = ready ? ExecuteBatch(MakeVecView(lists, listCount)) : unsubmitted;

        result.cause      = ready ? m_executionError
                                  : MakeRHIError(RHIErrorCode::eInvalidArgument, "RHI group preparation", __FILE__, __LINE__);

        if (!batch.result.cause.IsFailure() && result.cause.IsFailure())
        {
            batch.result.cause = result.cause;
        }

        result.accepted = {queue, GetAcceptedSerial(*commands, queue, before)};

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

        acceptedWork            |= result.accepted.serial > before;

        batch.result.submission  = result.submission;
    }

    return acceptedWork;
}

void RHICommandListExecutor::CancelBatch(const RefCountPtr<RHICommandBatch>& batch)
{
    if (!batch->executionFinished)
    {
        m_blocked.store(true, std::memory_order_release);

        batch->submissionState->Fail();

        batch->result.submission = RHISubmissionResult::eFatal;

        batch->result.cause      = GetTerminalError();

        if (!batch->result.cause.IsFailure())
        {
            batch->result.cause = MakeRHIError(RHIErrorCode::eCancelled, "RHI job cancelled", __FILE__, __LINE__);
        }

        batch->result.error = batch->result.cause.operation != nullptr ? batch->result.cause.operation : "RHI job cancelled";

        PublishTerminalError(batch->result.cause);

        batch->executionFinished = true;

        ++m_completedBatches;

        --m_pendingBatches;

        batch->completion.set_value(batch->result);

        batch->completionEvent->Signal();
    }
}

void RHICommandListExecutor::ExecuteFrame(const RefCountPtr<RHICommandBatch>& batch)
{
    GetRHIThread().CheckOwnership();

    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

    RHIBatchResult& result                            = batch->result;

    result.queueWaitUs  = std::chrono::duration<double, std::micro>(start - batch->queuedAt).count();

    bool beganExecution = false;

    if (!AreSubmissionsBlocked())
    {
        // Reserve retirement ownership before native execution can fail.
        m_retired.push_back(batch);

        beganExecution          = true;

        const bool acceptedWork = ExecuteGroups(*batch);

        if (result.submission == RHISubmissionResult::eSuccess && batch->viewport != nullptr)
        {
            // A frame whose last group is not graphics presents in its own submission.
            if (batch->presentCommands == nullptr)
            {
                batch->presentCommands = AcquirePresentCommandList();

                if (batch->presentCommands != nullptr)
                {
                    const RHIStatus prepared = batch->viewport->PrepareForPresentChecked(batch->presentCommands.get());

                    if (!prepared)
                    {
                        result.cause = prepared.error;
                    }

                    RHICommandList* present = batch->presentCommands.get();

                    const uint64_t before   = m_backend->GetLastSubmittedSerial(RHICommandContextType::eGraphics);

                    result.submission       = prepared ? ExecuteBatch(MakeVecView(&present, 1)) : RHISubmissionResult::eFatal;

                    if (!result.cause.IsFailure())
                    {
                        result.cause = m_executionError;
                    }

                    if (result.submission == RHISubmissionResult::eRejected
                        && (acceptedWork || m_backend->GetLastSubmittedSerial(RHICommandContextType::eGraphics) > before))
                    {
                        result.submission = RHISubmissionResult::eFatal;
                    }
                }
                else
                {
                    result.submission = acceptedWork ? RHISubmissionResult::eFatal : RHISubmissionResult::eRejected;
                }
            }

            if (result.submission == RHISubmissionResult::eSuccess)
            {
                result.presentation = batch->viewport->PresentChecked();

                result.presented    = result.presentation.presented;

                if (result.presentation.error.IsFailure())
                {
                    result.cause      = result.presentation.error;

                    result.submission = RHISubmissionResult::eFatal;
                }

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

        result.cause      = GetTerminalError();
    }

    if (result.submission != RHISubmissionResult::eSuccess && !result.cause.IsFailure())
    {
        result.cause = MakeRHIError(RHIErrorCode::eBackendFailure, "RHI frame submission", __FILE__, __LINE__);
    }

    // Once accepted asynchronously, rejection invalidates dependent scheduled state.
    // Never execute later batches against state that did not reach the GPU.
    if (result.submission != RHISubmissionResult::eSuccess)
    {
        batch->submissionState->Fail();

        if (result.error.empty())
        {
            result.error = result.submission == RHISubmissionResult::eRejected
                             ? "Native submission rejected an asynchronously queued frame"
                             : "Native submission failed after possible partial execution";
        }

        if (batch->endFrame || batch->viewport != nullptr || result.submission == RHISubmissionResult::eFatal)
        {
            PublishTerminalError(result.cause);
        }
    }

    PublishProgress();

    if (AreSubmissionsBlocked() && result.submission == RHISubmissionResult::eSuccess)
    {
        result.submission = RHISubmissionResult::eFatal;

        result.error      = "Backend failed while querying RHI submission progress";

        result.cause      = GetTerminalError();
    }

    result.requiredSerials.Extend(GetCachedSubmittedSerials());

    if (result.submission == RHISubmissionResult::eSuccess && !batch->submissionState->FinishSubmission())
    {
        result.submission = RHISubmissionResult::eFatal;

        result.error      = "Frame submission did not accept every scheduled producer";

        result.cause =
            MakeRHIError(RHIErrorCode::eBackendFailure, "Incomplete scheduled producer submission", __FILE__, __LINE__);

        PublishTerminalError(result.cause);
    }

    if (result.submission != RHISubmissionResult::eSuccess)
    {
        batch->submissionState->Fail();

        if (result.error.empty() && result.cause.operation != nullptr)
        {
            result.error = result.cause.operation;
        }
    }

    result.executionCPUUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();

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
    if (!AreSubmissionsBlocked())
    {
        m_backend->BeginFrame();

        PublishProgress();

        CollectCompletedBatches();
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

void RHICommandListExecutor::BeginGPUFrameTiming(const RHIGPUFrameTimingPtr& timing)
{
    GetRHIThread().Dispatch(std::bind_front(&DynamicRHI::BeginGPUFrameTiming, m_backend, timing), [timing] {
        if (timing)
        {
            timing->Seal(RHIGPUTimingStatus::eError);
        }
    });
}

void RHICommandListExecutor::EndGPUFrameTiming(const RHIGPUFrameTimingPtr& timing, bool succeeded)
{
    GetRHIThread().DispatchCleanup(std::bind_front(&RHICommandListExecutor::ExecuteEndGPUFrameTiming, this, timing, succeeded));
}

void RHICommandListExecutor::ExecuteEndGPUFrameTiming(const RHIGPUFrameTimingPtr& timing, bool succeeded)
{
    // Include failures discovered asynchronously after render-thread handoff.
    m_backend->EndGPUFrameTiming(timing, succeeded && !AreSubmissionsBlocked());
}

RHISubmissionResult RHICommandListExecutor::FlushAllGPUCommands()
{
    return GetRHIThread().Invoke(&RHICommandListExecutor::ExecuteBatch, this, VectorView<RHICommandList*>());
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

bool RHICommandListExecutor::ExecuteWaitForCompletion(RHICommandContextType type, uint64_t serial, uint64_t timeoutNS)
{
    const bool completed = m_backend->WaitForCompletion(type, serial, timeoutNS);

    PublishProgress();

    CollectCompletedBatches(false);

    return completed;
}

bool RHICommandListExecutor::WaitForCompletion(RHICommandContextType type, uint64_t serial, uint64_t timeoutNS)
{
    return GetRHIThread().Invoke(&RHICommandListExecutor::ExecuteWaitForCompletion, this, type, serial, timeoutNS);
}

uint64_t RHICommandListExecutor::GetLastSubmittedSerial(RHICommandContextType type) const
{
    return m_mode == RHIExecutionMode::eInline
             ? m_backend->GetLastSubmittedSerial(type)
             : m_queueProgress[static_cast<uint32_t>(type)]->submitted.load(std::memory_order_acquire);
}

uint64_t RHICommandListExecutor::QueryLastCompletedSerial(RHICommandContextType type)
{
    uint64_t completed = m_queueProgress[static_cast<uint32_t>(type)]->completed.load(std::memory_order_acquire);

    if (m_mode == RHIExecutionMode::eInline)
    {
        completed = m_backend->QueryLastCompletedSerial(type);

        PublishSubmissionStatus();
    }

    return completed;
}

void RHICommandListExecutor::RefreshGPUProgress()
{
    PublishProgress();

    CollectCompletedBatches();

    // Releasing command arenas can release the last recording of a bindless epoch.
    m_backend->CollectRetiredBindlessResources();

    PublishSubmissionStatus();
}

void RHICommandListExecutor::ExecutePollGPUProgress()
{
    RefreshGPUProgress();

    m_progressPollPending.store(false, std::memory_order_release);
}

void RHICommandListExecutor::PollGPUProgress()
{
    bool pending = false;

    if (m_progressPollPending.compare_exchange_strong(pending, true, std::memory_order_acq_rel))
    {
        if (!GetRHIThread().TryDispatch(std::bind_front(&RHICommandListExecutor::ExecutePollGPUProgress, this)))
        {
            m_progressPollPending.store(false, std::memory_order_release);
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
    return m_blocked.load(std::memory_order_acquire) || GetRHIThread().HasTaskFailure();
}

RHIThreadMetrics RHICommandListExecutor::GetThreadMetrics() const
{
    return {m_submittedBatches.load(), m_completedBatches.load(),   m_executionCPUUs.load(),          m_queueWaitUs.load(),
            m_pendingBatches.load(),   m_peakPendingBatches.load(), m_backend->GetExecutionCounters()};
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

RHIGPUMemoryStats RHICommandListExecutor::GetGPUMemoryStats() const
{
    // This backend API explicitly permits concurrent reads; do not enqueue an RHI wait.
    return m_backend->GetGPUMemoryStats();
}

RHIQueueCopyCapabilities RHICommandListExecutor::GetQueueCopyCapabilities(RHICommandContextType type) const
{
    return m_mode == RHIExecutionMode::eInline ? m_backend->GetQueueCopyCapabilities(type)
                                               : m_queueCapabilities[static_cast<uint32_t>(type)];
}

void RHICommandListExecutor::Init()
{
    GetRHIThread().Invoke(&DynamicRHI::Init, m_backend);
}

void RHICommandListExecutor::DestroyViewport(RHIViewport* viewport)
{
    GetRHIThread().Invoke(&DynamicRHI::DestroyViewport, m_backend, viewport);
}

RHIViewport* RHICommandListExecutor::CreateViewport(void* window, uint32_t width, uint32_t height, bool vsync)
{
    ASSERT(std::this_thread::get_id() == m_renderThread);

    return AreSubmissionsBlocked() ? nullptr : m_backend->CreateViewport(window, width, height, vsync);
}

RHIShader* RHICommandListExecutor::CreateShader(const RHIShaderCreateInfo& info)
{
    RHIResult<RHIShader*> result = GetRHIThread().InvokeChecked(&DynamicRHI::CreateShader, m_backend, std::cref(info));

    return result ? result.GetValue() : nullptr;
}

void RHICommandListExecutor::DestroyShader(RHIShader* shader)
{
    GetRHIThread().Invoke(&DynamicRHI::DestroyShader, m_backend, shader);
}

RHIPipeline* RHICommandListExecutor::CreatePipeline(const RHIComputePipelineCreateInfo& info)
{
    RHIResult<RHIPipeline*> result = GetRHIThread().InvokeChecked([&] { return m_backend->CreatePipeline(info); });

    return result ? result.GetValue() : nullptr;
}

RHIPipeline* RHICommandListExecutor::CreatePipeline(const RHIGfxPipelineCreateInfo& info)
{
    RHIResult<RHIPipeline*> result = GetRHIThread().InvokeChecked([&] { return m_backend->CreatePipeline(info); });

    return result ? result.GetValue() : nullptr;
}

void RHICommandListExecutor::DestroyPipeline(RHIPipeline* pipeline)
{
    GetRHIThread().Invoke(&DynamicRHI::DestroyPipeline, m_backend, pipeline);
}

RHISampler* RHICommandListExecutor::CreateSampler(const RHISamplerCreateInfo& info)
{
    RHIResult<RHISampler*> result = GetRHIThread().InvokeChecked(&DynamicRHI::CreateSampler, m_backend, std::cref(info));

    return result ? result.GetValue() : nullptr;
}

void RHICommandListExecutor::DestroySampler(RHISampler* sampler)
{
    GetRHIThread().Invoke(&DynamicRHI::DestroySampler, m_backend, sampler);
}

RHIBindlessHandle RHICommandListExecutor::RegisterBindlessResource(RHIResource* resource, uint32_t slot)
{
    RHIResult<RHIBindlessHandle> result =
        GetRHIThread().InvokeChecked(&DynamicRHI::RegisterBindlessResource, m_backend, resource, slot);

    return result ? result.GetValue() : RHIBindlessHandle{};
}

bool RHICommandListExecutor::UnregisterBindlessResource(RHIBindlessHandle handle)
{
    return GetRHIThread().Invoke(&DynamicRHI::UnregisterBindlessResource, m_backend, handle);
}

bool RHICommandListExecutor::IsBindlessResourceRegistered(RHIBindlessHandle handle)
{
    return GetRHIThread().Invoke(&DynamicRHI::IsBindlessResourceRegistered, m_backend, handle);
}

bool RHICommandListExecutor::ResetBindlessResources()
{
    const bool reset = GetRHIThread().Invoke(&DynamicRHI::ResetBindlessResources, m_backend);

    return reset;
}

void RHICommandListExecutor::CollectRetiredBindlessResources()
{
    GetRHIThread().Invoke(&RHICommandListExecutor::RefreshGPUProgress, this);
}

RHITexture* RHICommandListExecutor::CreateTexture(const RHITextureCreateInfo& info)
{
    RHIResult<RHITexture*> result = GetRHIThread().InvokeChecked(&DynamicRHI::CreateTexture, m_backend, std::cref(info));

    return result ? result.GetValue() : nullptr;
}

RHITextureView* RHICommandListExecutor::CreateTextureView(RHITexture* texture, const RHITextureViewCreateInfo& info)
{
    RHIResult<RHITextureView*> result =
        GetRHIThread().InvokeChecked(&DynamicRHI::CreateTextureView, m_backend, texture, std::cref(info));

    return result ? result.GetValue() : nullptr;
}

void RHICommandListExecutor::DestroyTexture(RHITexture* texture)
{
    GetRHIThread().Invoke(&DynamicRHI::DestroyTexture, m_backend, texture);
}

RHIBuffer* RHICommandListExecutor::CreateBuffer(const RHIBufferCreateInfo& info)
{
    RHIResult<RHIBuffer*> result = GetRHIThread().InvokeChecked(&DynamicRHI::CreateBuffer, m_backend, std::cref(info));

    return result ? result.GetValue() : nullptr;
}

void RHICommandListExecutor::DestroyBuffer(RHIBuffer* buffer)
{
    GetRHIThread().Invoke(&DynamicRHI::DestroyBuffer, m_backend, buffer);
}

IRHICommandContext* RHICommandListExecutor::GetCommandContext(RHICommandContextType type)
{
    RHIResult<IRHICommandContext*> result = GetRHIThread().InvokeChecked(&DynamicRHI::GetCommandContext, m_backend, type);

    return result ? result.GetValue() : nullptr;
}

IRHICommandContext* RHICommandListExecutor::GetTransferCommandContext()
{
    return GetCommandContext(RHICommandContextType::eTransfer);
}

RHIStatus RHICommandListExecutor::FinalizeCommandLists(VectorView<RHICommandList*>          lists,
                                                       HeapVector<RHIPlatformCommandList*>& output)
{
    return GetRHIThread().Invoke(&DynamicRHI::FinalizeCommandLists, m_backend, lists, std::ref(output));
}

void RHICommandListExecutor::SubmitPlatformCommandLists(VectorView<RHIPlatformCommandList*> lists)
{
    GetRHIThread().Invoke(&DynamicRHI::SubmitPlatformCommandLists, m_backend, lists);
}

RHITextureCopyCapabilities RHICommandListExecutor::GetTextureCopyCapabilities(DataFormat format) const
{
    return m_backend->GetTextureCopyCapabilities(format);
}
} // namespace zen
