#include "Utils/Errors.h"
TEST_F(RenderCoreTest, BufferHelpersRejectAllocationFailuresAndRetry)
{
    const uint8_t data[16]{};

    for (uint32_t kind = 0; kind < 5; ++kind)
    {
        rhi->failBufferCreationAt = rhi->bufferCreations + 1;
        RHIBuffer* failed         = nullptr;

        switch (kind)
        {
            case 0: failed = device->CreateVertexBuffer(sizeof(data), data); break;
            case 1: failed = device->CreateIndexBuffer(sizeof(data), data); break;
            case 2: failed = device->CreateUniformBuffer(sizeof(data), data, "failed_uniform"); break;
            case 3: failed = device->CreateStorageBuffer(sizeof(data), data, "failed_storage"); break;
            case 4: failed = device->CreateIndirectBuffer(sizeof(data), data, "failed_indirect"); break;
        }

        EXPECT_EQ(failed, nullptr);
    }

    RHIBuffer* retry = device->CreateStorageBuffer(sizeof(data), data, "retry_storage");
    ASSERT_NE(retry, nullptr);

    device->DestroyBuffer(retry);
}

TEST_F(RenderCoreTest, StagingAllocationFailureDoesNotPublishAnInvalidBlock)
{
    StagingBufferManager manager(64, 128);
    StagingAllocation    allocation;
    rhi->failBufferCreationAt = rhi->bufferCreations + 1;

    EXPECT_EQ(manager.Allocate(16, 4, &allocation), StagingFlushAction::eFailed);
    EXPECT_EQ(allocation.pBuffer, nullptr);
    EXPECT_EQ(manager.Allocate(16, 4, &allocation), StagingFlushAction::eNone);
    ASSERT_NE(allocation.pBuffer, nullptr);

    manager.Release(allocation, {});
    manager.Destroy();
}

TEST(RHIGPUMemoryBudgetTest, PressureUsesDeviceLocalBudgetAndHandlesUnavailableTelemetry)
{
    RHIGPUMemoryStats stats;
    stats.heapCount = 2;
    stats.heaps[0]  = {1000, 950, 1000, false};
    stats.heaps[1]  = {1000, 900, 1000, true};
    EXPECT_FALSE(stats.IsUnderPressure());

    stats.budgetAvailable = true;
    EXPECT_TRUE(stats.IsUnderPressure());
    stats.heaps[1].usageBytes = 899;
    EXPECT_FALSE(stats.IsUnderPressure());
    stats.heaps[1].budgetBytes = 0;
    EXPECT_FALSE(stats.IsUnderPressure());
}

TEST(RHIIndexOffsetTest, CommandsAndGraphDescriptorsPreserveOffsetsBeyondFourGiB)
{
    constexpr uint64_t           offset = (uint64_t(1) << 32) + 256;
    RHICommandDrawIndexed::Param direct{};
    direct.indexBufferOffset = offset;
    RHICommandDrawIndexed directCommand(direct, 0);
    EXPECT_EQ(directCommand.indexBufferOffset, offset);

    RHICommandDrawIndexedIndirect::Param indirect{};
    indirect.indexBufferOffset = offset;
    RHICommandDrawIndexedIndirect indirectCommand(indirect, 0);
    EXPECT_EQ(indirectCommand.indexBufferOffset, offset);

    RDGGraphicsPassDesc pass;
    pass.BindIndexBuffer(static_cast<RHIBuffer*>(nullptr), DataFormat::eR32UInt, offset);
    EXPECT_EQ(pass.geometryBuffer.indexBufferOffset, offset);
}

TEST(RHIThreadProductionTest, TaskFailureCancelsNormalWorkAndPreservesCleanup)
{
    for (RHIExecutionMode mode : {RHIExecutionMode::eInline, RHIExecutionMode::eThreaded})
    {
        RHIThread thread;
        thread.Start(mode);
        uint32_t cancellations = 0;
        uint32_t cleanups      = 0;
        uint32_t normal        = 0;
        thread.Dispatch([&thread] {
            thread.Fail(MakeRHIError(RHIErrorCode::eBackendFailure, "injected task failure", __FILE__, __LINE__));
        });
        thread.Dispatch([&normal] { ++normal; }, [&cancellations] { ++cancellations; });
        EXPECT_TRUE(thread.DispatchCleanup([&cleanups] { ++cleanups; }));
        thread.Stop([&cleanups] { ++cleanups; });
        EXPECT_TRUE(thread.HasTaskFailure());
        EXPECT_EQ(cancellations, 1u);
        EXPECT_EQ(cleanups, 2u);
        EXPECT_EQ(normal, 0u);
    }
}

TEST(RHIThreadProductionTest, ShutdownDrainsLateCleanupBeforeTheFinalizer)
{
    RHIThread            thread;
    RHIThreadEvent       entered;
    RHIThreadEvent       release;
    HeapVector<uint32_t> order;
    thread.Start(RHIExecutionMode::eThreaded);
    thread.Dispatch([&entered, &release] {
        entered.Signal();
        release.Wait();
    });
    entered.Wait();

    std::thread stopper([&thread, &order] { thread.Stop([&order] { order.push_back(2); }); });

    while (!thread.IsStopping())
    {
        std::this_thread::yield();
    }

    uint32_t cancellations = 0;
    EXPECT_FALSE(thread.Dispatch([] {}, [&cancellations] { ++cancellations; }));
    EXPECT_TRUE(thread.DispatchCleanup([&order] { order.push_back(1); }));
    release.Signal();
    stopper.join();
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], 1u);
    EXPECT_EQ(order[1], 2u);
    EXPECT_EQ(cancellations, 1u);
}

TEST(RHIThreadProductionTest, FailedWorkerCompletesQueuedFrameTiming)
{
    for (RHIExecutionMode mode : {RHIExecutionMode::eInline, RHIExecutionMode::eThreaded})
    {
        RHICommandListExecutor executor(ZEN_NEW() TestRHI(), mode);
        RHIGPUFrameTimingPtr   timing = MakeShared<RHIGPUFrameTiming, MultiThreadCounter>();

        GetRHIThread().Dispatch([] {
            GetRHIThread().Fail(
                MakeRHIError(RHIErrorCode::eBackendFailure, "injected timing task failure", __FILE__, __LINE__));
        });
        executor.BeginGPUFrameTiming(timing);
        executor.EndGPUFrameTiming(timing, true);
        executor.FlushRHIThread();

        EXPECT_EQ(timing->GetStatus(), RHIGPUTimingStatus::eError);
        EXPECT_TRUE(executor.AreSubmissionsBlocked());
    }
}

TEST_F(RHIExecutorTest, CancelledQueuedBatchCompletesItsTicketAfterTaskFailure)
{
    RHICommandListPtr commands(RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));
    RHIThreadEvent    entered;
    RHIThreadEvent    release;

    GetRHIThread().Dispatch([&entered, &release] {
        entered.Signal();
        release.Wait();
    });
    entered.Wait();

    GetRHIThread().Dispatch([] {
        GetRHIThread().Fail(MakeRHIError(RHIErrorCode::eBackendFailure, "injected batch task failure", __FILE__, __LINE__));
    });
    const RHISubmissionTicket ticket = executor->SubmitFrame(*commands, nullptr);
    release.Signal();

    ASSERT_TRUE(ticket.IsValid());
    EXPECT_EQ(ticket.Wait().submission, RHISubmissionResult::eFatal);
    EXPECT_EQ(ticket.Wait().submission, RHISubmissionResult::eFatal);
    EXPECT_TRUE(executor->AreSubmissionsBlocked());
    EXPECT_EQ(rhi->submissionAttempts, 0u);
}

TEST_F(RHIExecutorTest, MissingPresentationListStopsRequiredFrame)
{
    TestViewport viewport;

    const SmallVector<RHICommandContextType, 1> queues{RHICommandContextType::eGraphics};

    TestOwnedSchedule rejected(*executor, queues);

    rejected.lists[0]->Draw(3, 1, 0, 0);

    // The executor creates its first presentation list on demand; that creation fails.
    rhi->failContextCreation    = true;

    const RHIBatchResult result = executor->SubmitGroups(rejected.groups, rejected.state, &viewport);

    EXPECT_EQ(result.submission, RHISubmissionResult::eRejected);

    EXPECT_EQ(rhi->submissionAttempts, 0u);

    EXPECT_EQ(viewport.preparePresents, 0u);

    EXPECT_TRUE(executor->AreSubmissionsBlocked());

    EXPECT_TRUE(result.cause.IsFailure());

    EXPECT_EQ(viewport.presents, 0u);

    // Retained batches reference the stack viewport; release them before it leaves scope.
    rejected.lists.clear();

    viewport.ReleaseForTeardown();

    executor->Destroy();
}

TEST(RHIThreadProductionTest, FullQueueFailureWakesProducersAndPreservesTheFirstCause)
{
    RHIThread thread;

    thread.Start(RHIExecutionMode::eThreaded, 1);

    RHIThreadEvent entered;

    RHIThreadEvent release;

    ASSERT_TRUE(thread.Dispatch([&entered, &release] {
        entered.Signal();

        release.Wait();
    }));

    entered.Wait();

    uint32_t cancelled = 0;

    uint32_t executed  = 0;

    EXPECT_TRUE(thread.Dispatch([&executed] { ++executed; }, [&cancelled] { ++cancelled; }));

    EXPECT_EQ(thread.DispatchChecked([] {}, {}, false).admission, RHIJobAdmission::eQueueFull);

    RHIJobAdmissionResult producerResult;

    RHIThreadEvent producerStarted;

    std::thread producer([&thread, &producerResult, &producerStarted] {
        producerStarted.Signal();

        producerResult = thread.DispatchChecked([] {});
    });

    producerStarted.Wait();

    thread.Fail({RHIErrorCode::eOutOfDeviceMemory, -2, "first failure", __FILE__, 7, 91});

    thread.Fail({RHIErrorCode::eDeviceLost, -4, "later device loss", __FILE__, 8});

    producer.join();

    EXPECT_EQ(producerResult.admission, RHIJobAdmission::eFailed);

    EXPECT_EQ(producerResult.error.resourceId, 91u);

    EXPECT_TRUE(thread.HasDeviceLoss());

    EXPECT_STREQ(thread.GetFailure().operation, "first failure");

    release.Signal();

    thread.Flush();

    EXPECT_EQ(cancelled, 1u);

    EXPECT_EQ(executed, 0u);

    thread.Stop();
}

TEST(RHIThreadProductionTest, CheckedInvocationReturnsMoveOnlyValuesAndCancellationWithoutWaiting)
{
    for (RHIExecutionMode mode : {RHIExecutionMode::eInline, RHIExecutionMode::eThreaded})
    {
        RHIThread thread;

        thread.Start(mode);

        RHIResult<UniquePtr<int>> value = thread.InvokeChecked([] { return MakeUnique<int>(17); });

        ASSERT_TRUE(value);

        EXPECT_EQ(*value.GetValue(), 17);

        EXPECT_TRUE(thread.InvokeChecked([] {}));

        thread.Fail({RHIErrorCode::eBackendFailure, -99, "invoke cancellation", __FILE__, 12});

        uint32_t calls           = 0;

        RHIResult<int> cancelled = thread.InvokeChecked([&calls] {
            ++calls;
            return 42;
        });

        EXPECT_FALSE(cancelled);

        EXPECT_EQ(cancelled.GetError().nativeCode, -99);

        EXPECT_EQ(calls, 0u);

        EXPECT_TRUE(thread.DispatchCleanup([&calls] { ++calls; }));

        thread.Stop([] {});

        EXPECT_EQ(calls, 1u);

        EXPECT_EQ(thread.DispatchChecked([] {}).admission, RHIJobAdmission::eFailed);
    }
}

class FailingFinalizeRHI : public TestRHI
{
public:
    RHIStatus FinalizeCommandLists(VectorView<RHICommandList*>, HeapVector<RHIPlatformCommandList*>&) override
    {
        return {{RHIErrorCode::eOutOfDeviceMemory, -2, "injected vkEndCommandBuffer", __FILE__, 73, 19}};
    }
};

TEST(RHIThreadProductionTest, FrameTicketPreservesFinalizationCauseAndBlocksNewResources)
{
    for (RHIExecutionMode mode : {RHIExecutionMode::eInline, RHIExecutionMode::eThreaded})
    {
        RHICommandListExecutor executor(ZEN_NEW() FailingFinalizeRHI(), mode);

        RHICommandListPtr commands(RHICommandList::Create(executor.GetCommandContext(RHICommandContextType::eGraphics)));

        commands->Draw(3, 1, 0, 0);

        const RHISubmissionTicket ticket = executor.SubmitFrame(*commands, nullptr);

        ASSERT_TRUE(ticket.IsValid());

        const RHIBatchResult& result = ticket.Wait();

        EXPECT_EQ(result.submission, RHISubmissionResult::eRejected);

        EXPECT_EQ(result.cause.nativeCode, -2);

        EXPECT_EQ(result.cause.line, 73u);

        EXPECT_EQ(result.cause.resourceId, 19u);

        EXPECT_STREQ(executor.GetTerminalError().operation, "injected vkEndCommandBuffer");

        const RHIProgressResult progress = executor.QueryProgressChecked();

        EXPECT_EQ(progress.error.nativeCode, -2);

        EXPECT_EQ(executor.WaitForCompletionChecked(RHICommandContextType::eGraphics, 1, 0).outcome, RHIWaitOutcome::eFailed);

        EXPECT_EQ(executor.CreateBuffer({}), nullptr);

        commands.reset();

        executor.Destroy();
    }
}
