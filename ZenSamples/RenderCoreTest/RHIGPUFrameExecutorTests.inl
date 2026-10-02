// Included after RHIThreadingTests.inl to reuse its fake backend and submission gate.
namespace
{
class FrameTimingTestRHI : public TestRHI
{
public:
    enum class Event
    {
        eBegin,
        eSubmit,
        eEnd
    };

    void BeginGPUFrameTiming(const RHIGPUFrameTimingPtr& timing) override
    {
        events.push_back(Event::eBegin);

        beginThread = std::this_thread::get_id();

        ++recordings;

        active = timing;
    }

    RHISubmissionResult FlushAllGPUCommands() override
    {
        const bool hasWork = !pendingCommandLists.empty();

        RHIGPUTimingPtr interval;

        if (hasWork && active)
        {
            events.push_back(Event::eSubmit);

            submitThread = std::this_thread::get_id();

            interval     = active->AddInterval();
        }

        const RHISubmissionResult result = TestRHI::FlushAllGPUCommands();

        if (interval)
        {
            if (result == RHISubmissionResult::eSuccess || submitBeforeFailure)
            {
                const uint64_t begin = recordings * 100;

                interval->PublishTimestamps({begin, begin + recordings * 50, 64, 1000});
            }
            else
            {
                interval->Publish(RHIGPUTimingStatus::eDiscarded);
            }
        }

        return result;
    }

    void EndGPUFrameTiming(const RHIGPUFrameTimingPtr& timing, bool succeeded) override
    {
        events.push_back(Event::eEnd);

        endThread    = std::this_thread::get_id();

        endSucceeded = succeeded;

        EXPECT_EQ(timing, active);

        if (timing)
        {
            timing->Seal(succeeded ? RHIGPUTimingStatus::eAvailable : RHIGPUTimingStatus::eError);
        }

        active.Reset();
    }

    HeapVector<Event>    events;
    std::thread::id      beginThread;
    std::thread::id      submitThread;
    std::thread::id      endThread;
    RHIGPUFrameTimingPtr active;
    uint64_t             recordings{0};
    bool                 endSucceeded{false};
};

class RHIGPUFrameExecutorTest : public RHIExecutorTest
{
protected:
    void SetUp() override
    {
        destroyed.clear();

        GRHIFrameState.Init(3);

        timingRHI   = ZEN_NEW() FrameTimingTestRHI();

        rhi         = timingRHI;

        executor    = ZEN_NEW() RHICommandListExecutor(rhi, RHIExecutionMode::eThreaded);

        GDynamicRHI = executor;
    }

    void VerifySubmissionFailure(RHISubmissionResult failure, bool partial)
    {
        RHICommandListPtr commands(RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));

        commands->Draw(3, 1, 0, 0);

        rhi->failSubmissionAt    = 1;

        rhi->submissionFailure   = failure;

        rhi->submitBeforeFailure = partial;

        ArmGate();

        RHIGPUFrameTimingPtr timing = MakeShared<RHIGPUFrameTiming, MultiThreadCounter>();

        executor->BeginGPUFrameTiming(timing);

        const RHISubmissionTicket ticket = executor->SubmitFrame(*commands, nullptr);

        EXPECT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);

        EXPECT_TRUE(ticket.IsValid());

        EXPECT_FALSE(ticket.IsReady());

        // CPU handoff succeeded, but the worker has not discovered the native failure yet.
        executor->EndGPUFrameTiming(timing, true);

        EXPECT_EQ(timing->GetStatus(), RHIGPUTimingStatus::ePending);

        gate.Open();

        EXPECT_EQ(ticket.Wait().submission, failure);

        executor->FlushRHIThread();

        EXPECT_TRUE(gate.releasedInTime);

        EXPECT_TRUE(executor->AreSubmissionsBlocked());

        EXPECT_FALSE(timingRHI->endSucceeded);

        EXPECT_EQ(timing->GetStatus(), RHIGPUTimingStatus::eError);

        EXPECT_DOUBLE_EQ(timing->GetMicroseconds(), 0);

        EXPECT_EQ(rhi->graphics.drawCount, partial ? 1u : 0u);

        EXPECT_TRUE(rhi->submissionWaits.empty());

        EXPECT_EQ(rhi->deviceIdleWaits, 0u);
    }

    FrameTimingTestRHI* timingRHI{nullptr};
};
} // namespace

TEST_F(RHIGPUFrameExecutorTest, FrameMarkersQueueInOrderWithoutWaitingForTheWorker)
{
    RHICommandListPtr commands(RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));

    commands->Draw(3, 1, 0, 0);

    ArmGate();

    GetRHIThread().Dispatch(std::bind_front(&RHISubmissionGate::OnSubmission, &gate));

    EXPECT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);

    RHIGPUFrameTimingPtr timing = MakeShared<RHIGPUFrameTiming, MultiThreadCounter>();

    executor->BeginGPUFrameTiming(timing);

    const RHISubmissionTicket ticket = executor->SubmitFrame(*commands, nullptr);

    executor->EndGPUFrameTiming(timing, true);

    EXPECT_FALSE(ticket.IsReady());

    EXPECT_EQ(timing->GetStatus(), RHIGPUTimingStatus::ePending);

    gate.Open();

    EXPECT_EQ(ticket.Wait().submission, RHISubmissionResult::eSuccess);

    executor->FlushRHIThread();

    EXPECT_TRUE(gate.releasedInTime);

    EXPECT_EQ(timing->GetStatus(), RHIGPUTimingStatus::eAvailable);

    EXPECT_DOUBLE_EQ(timing->GetMicroseconds(), 50);

    EXPECT_EQ(timingRHI->events.size(), 3u);

    if (timingRHI->events.size() == 3)
    {
        EXPECT_EQ(timingRHI->events[0], FrameTimingTestRHI::Event::eBegin);

        EXPECT_EQ(timingRHI->events[1], FrameTimingTestRHI::Event::eSubmit);

        EXPECT_EQ(timingRHI->events[2], FrameTimingTestRHI::Event::eEnd);
    }

    EXPECT_EQ(timingRHI->beginThread, gate.executionThread);

    EXPECT_EQ(timingRHI->submitThread, gate.executionThread);

    EXPECT_EQ(timingRHI->endThread, gate.executionThread);

    EXPECT_NE(timingRHI->beginThread, std::this_thread::get_id());

    EXPECT_TRUE(rhi->submissionWaits.empty());

    EXPECT_EQ(rhi->deviceIdleWaits, 0u);
}

TEST_F(RHIGPUFrameExecutorTest, RejectionDiscoveredAfterHandoffInvalidatesTheFrame)
{
    VerifySubmissionFailure(RHISubmissionResult::eRejected, false);
}

TEST_F(RHIGPUFrameExecutorTest, PartialFatalSubmissionCannotReportACompleteFrame)
{
    VerifySubmissionFailure(RHISubmissionResult::eFatal, true);
}

TEST_F(RHIGPUFrameExecutorTest, CallerFailureInvalidatesOtherwiseSuccessfulNativeWork)
{
    RHICommandListPtr commands(RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));

    commands->Draw(3, 1, 0, 0);

    RHIGPUFrameTimingPtr timing = MakeShared<RHIGPUFrameTiming, MultiThreadCounter>();

    executor->BeginGPUFrameTiming(timing);

    const RHISubmissionTicket ticket = executor->SubmitFrame(*commands, nullptr);

    executor->EndGPUFrameTiming(timing, false);

    EXPECT_EQ(ticket.Wait().submission, RHISubmissionResult::eSuccess);

    executor->FlushRHIThread();

    EXPECT_FALSE(executor->AreSubmissionsBlocked());

    EXPECT_FALSE(timingRHI->endSucceeded);

    EXPECT_EQ(timing->GetStatus(), RHIGPUTimingStatus::eError);

    EXPECT_DOUBLE_EQ(timing->GetMicroseconds(), 0);
}

TEST(RHIGPUFrameExecutor, ReplayUsesFreshCapturesInInlineAndThreadedModes)
{
    for (const RHIExecutionMode mode : {RHIExecutionMode::eInline, RHIExecutionMode::eThreaded})
    {
        FrameTimingTestRHI* backend = ZEN_NEW() FrameTimingTestRHI();

        GRHIFrameState.Init(3);

        {
            RHICommandListExecutor executor(backend, mode);

            GDynamicRHI = &executor;

            RHICommandListPtr commands(RHICommandList::Create(executor.GetCommandContext(RHICommandContextType::eGraphics)));

            commands->Draw(3, 1, 0, 0);

            RHICommandList* replay = commands.get();

            HeapVector<RHIGPUFrameTimingPtr> captures;

            for (uint32_t frame = 0; frame < 2; ++frame)
            {
                RHIGPUFrameTimingPtr timing = MakeShared<RHIGPUFrameTiming, MultiThreadCounter>();

                executor.BeginGPUFrameTiming(timing);

                EXPECT_EQ(executor.SubmitBatch(MakeVecView(&replay, 1)), RHISubmissionResult::eSuccess);

                executor.EndGPUFrameTiming(timing, true);

                executor.FlushRHIThread();

                EXPECT_EQ(timing->GetStatus(), RHIGPUTimingStatus::eAvailable);

                EXPECT_DOUBLE_EQ(timing->GetMicroseconds(), (frame + 1) * 50);

                captures.push_back(timing);
            }

            EXPECT_DOUBLE_EQ(captures[0]->GetMicroseconds(), 50);

            EXPECT_DOUBLE_EQ(captures[1]->GetMicroseconds(), 100);

            EXPECT_NE(captures[0], captures[1]);

            EXPECT_EQ(backend->graphics.drawCount, 2u);

            EXPECT_EQ(backend->beginThread == std::this_thread::get_id(), mode == RHIExecutionMode::eInline);

            EXPECT_EQ(backend->beginThread, backend->endThread);

            EXPECT_TRUE(backend->submissionWaits.empty());

            EXPECT_EQ(backend->deviceIdleWaits, 0u);

            commands.reset();

            executor.Destroy();
        }

        GDynamicRHI = nullptr;
    }
}

TEST_F(RHIExecutorTest, BackendWithoutFrameTimestampsReportsUnsupported)
{
    RHIGPUFrameTimingPtr timing = MakeShared<RHIGPUFrameTiming, MultiThreadCounter>();

    executor->BeginGPUFrameTiming(timing);

    executor->EndGPUFrameTiming(timing, true);

    executor->FlushRHIThread();

    EXPECT_EQ(timing->GetStatus(), RHIGPUTimingStatus::eUnsupported);

    EXPECT_DOUBLE_EQ(timing->GetMicroseconds(), 0);

    EXPECT_EQ(timing->GetIntervalCount(), 0u);

    EXPECT_TRUE(rhi->submissionWaits.empty());

    EXPECT_EQ(rhi->deviceIdleWaits, 0u);
}
