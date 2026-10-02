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
            case 2:
                failed = device->CreateUniformBuffer(sizeof(data), data, "failed_uniform");
                break;
            case 3:
                failed = device->CreateStorageBuffer(sizeof(data), data, "failed_storage");
                break;
            case 4:
                failed = device->CreateIndirectBuffer(sizeof(data), data, "failed_indirect");
                break;
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
    StagingAllocation allocation;
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
    constexpr uint64_t offset = (uint64_t(1) << 32) + 256;
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

TEST(RHIThreadProductionTest, TaskExceptionsCancelNormalWorkAndPreserveCleanup)
{
    for (RHIExecutionMode mode : {RHIExecutionMode::eInline, RHIExecutionMode::eThreaded})
    {
        RHIThread thread;
        thread.Start(mode);
        uint32_t cancellations = 0;
        uint32_t cleanups      = 0;
        uint32_t normal        = 0;
        thread.Dispatch([] { throw std::runtime_error("injected task failure"); },
                        [&cancellations] { ++cancellations; });
        thread.Dispatch([&normal] { ++normal; }, [&cancellations] { ++cancellations; });
        EXPECT_TRUE(thread.DispatchCleanup([&cleanups] { ++cleanups; }));
        thread.Stop([&cleanups] { ++cleanups; });
        EXPECT_TRUE(thread.HasTaskFailure());
        EXPECT_EQ(cancellations, 2u);
        EXPECT_EQ(cleanups, 2u);
        EXPECT_EQ(normal, 0u);
    }
}

TEST(RHIThreadProductionTest, ShutdownDrainsLateCleanupBeforeTheFinalizer)
{
    RHIThread thread;
    RHIThreadEvent entered;
    RHIThreadEvent release;
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
        RHIGPUFrameTimingPtr timing = MakeShared<RHIGPUFrameTiming, MultiThreadCounter>();

        GetRHIThread().Dispatch([] { throw std::runtime_error("injected timing task failure"); });
        executor.BeginGPUFrameTiming(timing);
        executor.EndGPUFrameTiming(timing, true);
        executor.FlushRHIThread();

        EXPECT_EQ(timing->GetStatus(), RHIGPUTimingStatus::eError);
        EXPECT_TRUE(executor.AreSubmissionsBlocked());
    }
}

TEST_F(RHIExecutorTest, CancelledQueuedBatchCompletesItsTicketAfterTaskFailure)
{
    RHICommandListPtr commands(
        RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));
    RHIThreadEvent entered;
    RHIThreadEvent release;

    GetRHIThread().Dispatch([&entered, &release] {
        entered.Signal();
        release.Wait();
    });
    entered.Wait();

    GetRHIThread().Dispatch([] { throw std::runtime_error("injected batch task failure"); });
    const RHISubmissionTicket ticket = executor->SubmitFrame(*commands, nullptr);
    release.Signal();

    ASSERT_TRUE(ticket.IsValid());
    EXPECT_EQ(ticket.Wait().submission, RHISubmissionResult::eFatal);
    EXPECT_EQ(ticket.Wait().submission, RHISubmissionResult::eFatal);
    EXPECT_TRUE(executor->AreSubmissionsBlocked());
    EXPECT_EQ(rhi->submissionAttempts, 0u);
}

TEST_F(RHIExecutorTest, MissingPresentationListRejectsTheFrameWithoutBlocking)
{
    TestViewport viewport;

    const SmallVector<RHICommandContextType, 1> queues{RHICommandContextType::eGraphics};

    TestOwnedSchedule rejected(*executor, queues);

    rejected.lists[0]->Draw(3, 1, 0, 0);

    // The executor creates its first presentation list on demand; that creation fails.
    rhi->failContextCreation = true;

    const RHIBatchResult result =
        executor->SubmitGroups(rejected.groups, rejected.state, &viewport);

    EXPECT_EQ(result.submission, RHISubmissionResult::eRejected);

    EXPECT_EQ(rhi->submissionAttempts, 0u);

    EXPECT_EQ(viewport.preparePresents, 0u);

    EXPECT_FALSE(executor->AreSubmissionsBlocked());

    // Nothing reached the GPU, so the rebuilt frame submits and presents.
    rhi->failContextCreation = false;

    TestOwnedSchedule retry(*executor, queues);

    retry.lists[0]->Draw(3, 1, 0, 0);

    EXPECT_EQ(executor->SubmitGroups(retry.groups, retry.state, &viewport).submission,
              RHISubmissionResult::eSuccess);

    EXPECT_EQ(viewport.presents, 1u);

    // Retained batches reference the stack viewport; release them before it leaves scope.
    executor->Destroy();
}
