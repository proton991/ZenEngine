namespace
{
struct TestOwnedSchedule
{
    HeapVector<RHICommandListPtr> lists;
    HeapVector<RHISubmissionGroup> groups;
    RefCountPtr<RHISubmissionState> state;

    TestOwnedSchedule(RHICommandListExecutor& executor,
                      VectorView<const RHICommandContextType> queues)
    {
        state = MakeRefCountPtr<RHISubmissionState>(queues);
        for (RHICommandContextType queue : queues)
        {
            lists.emplace_back(RHICommandList::Create(executor.GetCommandContext(queue)));
            groups.push_back({lists.back().get()});
        }
    }

    void Depends(uint32_t consumer, uint32_t producer)
    {
        groups[consumer].predecessors.push_back(
            {lists[producer]->GetContext()->GetContextType(), 0, state, producer});
    }
};

class RHIScheduledSubmissionTest : public testing::TestWithParam<std::tuple<RHIExecutionMode, bool>>
{
protected:
    void SetUp() override
    {
        destroyed.clear();
        GRHIFrameState.Init(3);
        rhi                                          = ZEN_NEW() TestRHI();
        rhi->asyncDependencies                       = true;
        rhi->submissionQueueCapabilities             = DistinctComputeQueues();
        rhi->submissionQueueCapabilities.queueIds[2] = std::get<1>(GetParam()) ? 1 : 2;
        executor    = ZEN_NEW() RHICommandListExecutor(rhi, std::get<0>(GetParam()));
        GDynamicRHI = executor;
    }

    void TearDown() override
    {
        executor->Destroy();
        ZEN_DELETE(executor);
        GDynamicRHI = nullptr;
    }

    RHIResourcePtr<RHIBuffer> Buffer()
    {
        RHIBufferCreateInfo info;
        info.size = 64;
        return RHIResourcePtr<RHIBuffer>(executor->CreateBuffer(info), false);
    }

    TestRHI* rhi{nullptr};
    RHICommandListExecutor* executor{nullptr};
    TestViewport viewport;
};

TEST_P(RHIScheduledSubmissionTest, ReadyGroupsSubmitInOrderAndRetainExactLogicalSerials)
{
    const SmallVector<RHICommandContextType, 4> queues{
        RHICommandContextType::eGraphics, RHICommandContextType::eAsyncCompute,
        RHICommandContextType::eGraphics, RHICommandContextType::eTransfer};
    TestOwnedSchedule schedule(*executor, queues);
    RHIResourcePtr<RHIBuffer> prefix = Buffer(), source = Buffer(), produced = Buffer(),
                              graphics = Buffer(), transfer = Buffer();
    TestBuffer* input = static_cast<TestBuffer*>(source.Get());
    for (size_t i = 0; i < input->bytes.size(); ++i)
    {
        input->bytes[i] = uint8_t(i + 7);
    }
    schedule.lists[0]->ClearBuffer(prefix.Get(), 0, 64);
    schedule.lists[1]->CopyBuffer(source.Get(), produced.Get(), {0, 0, 64});
    schedule.lists[2]->CopyBuffer(produced.Get(), graphics.Get(), {0, 0, 64});
    schedule.lists[3]->CopyBuffer(produced.Get(), transfer.Get(), {0, 0, 64});
    schedule.Depends(2, 1);
    schedule.Depends(3, 1);
    const RHIBatchResult result =
        executor->SubmitFrame(schedule.groups, &viewport, schedule.state).Wait();
    ASSERT_EQ(result.submission, RHISubmissionResult::eSuccess);
    ASSERT_EQ(result.groups.size(), 4u);
    EXPECT_EQ(result.groups[0].accepted.serial, 1u);
    EXPECT_EQ(result.groups[1].accepted.serial, 1u);
    EXPECT_EQ(result.groups[2].accepted.serial, 2u);
    EXPECT_EQ(result.groups[3].accepted.serial, 1u);
    EXPECT_EQ(result.groups[3].accepted.queue, RHICommandContextType::eTransfer);
    EXPECT_EQ(ToValue(GetRHIThread().Invoke(&RHIFrameState::GetFrameNumber, &GRHIFrameState)), 1u);
    EXPECT_EQ(viewport.presents, 1u);
    ASSERT_EQ(rhi->gpuDependencies.size(), std::get<1>(GetParam()) ? 1u : 2u);
    EXPECT_EQ(rhi->gpuDependencies[0].consumer, RHICommandContextType::eGraphics);
    EXPECT_EQ(rhi->gpuDependencies[0].producer.serial, 1u);
    EXPECT_TRUE(rhi->submissionWaits.empty());
    for (const RHICommandListPtr& list : schedule.lists)
    {
        EXPECT_EQ(list->GetCommandCount(), 0u);
    }
    EXPECT_EQ(static_cast<TestBuffer*>(graphics.Get())->bytes, input->bytes);
    EXPECT_EQ(static_cast<TestBuffer*>(transfer.Get())->bytes, input->bytes);
}

TEST_P(RHIScheduledSubmissionTest, InvalidScheduleDoesNotDetachAnyCommands)
{
    const SmallVector<RHICommandContextType, 2> queues{RHICommandContextType::eGraphics,
                                                       RHICommandContextType::eAsyncCompute};
    TestOwnedSchedule schedule(*executor, queues);
    schedule.lists[0]->Dispatch(1, 1, 1);
    schedule.lists[1]->Dispatch(1, 1, 1);
    schedule.Depends(0, 1);
    EXPECT_FALSE(executor->SubmitFrame(schedule.groups, nullptr, schedule.state).IsValid());
    schedule.groups[0].predecessors.clear();
    schedule.groups[1].commands = schedule.lists[0].get();
    EXPECT_FALSE(executor->SubmitFrame(schedule.groups, nullptr, schedule.state).IsValid());
    EXPECT_FALSE(schedule.state->IsQueued());
    EXPECT_EQ(schedule.lists[0]->GetCommandCount(), 1u);
    EXPECT_EQ(schedule.lists[1]->GetCommandCount(), 1u);
    EXPECT_EQ(rhi->submissionAttempts, 0u);
}

TEST_P(RHIScheduledSubmissionTest, AcceptedPrefixFailureStopsRemainingGroupsAndPresentation)
{
    const SmallVector<RHICommandContextType, 3> queues{RHICommandContextType::eAsyncCompute,
                                                       RHICommandContextType::eGraphics,
                                                       RHICommandContextType::eTransfer};
    TestOwnedSchedule schedule(*executor, queues);
    for (RHICommandListPtr& list : schedule.lists)
    {
        list->Dispatch(1, 1, 1);
    }
    schedule.Depends(1, 0);
    schedule.Depends(2, 1);
    rhi->failSubmissionAt = 2;
    const RHIBatchResult result =
        executor->SubmitFrame(schedule.groups, &viewport, schedule.state).Wait();
    EXPECT_EQ(result.submission, RHISubmissionResult::eFatal);
    ASSERT_EQ(result.groups.size(), 3u);
    EXPECT_EQ(result.groups[0].submission, RHISubmissionResult::eSuccess);
    EXPECT_EQ(result.groups[0].accepted.serial, 1u);
    EXPECT_EQ(result.groups[1].submission, RHISubmissionResult::eFatal);
    EXPECT_EQ(result.groups[2].submission, RHISubmissionResult::eRejected);
    EXPECT_EQ(rhi->submissionAttempts, 2u);
    EXPECT_EQ(viewport.preparePresents, 0u);
    EXPECT_EQ(viewport.presents, 0u);
    EXPECT_TRUE(executor->AreSubmissionsBlocked());
    RHISubmissionDependency point;
    EXPECT_EQ(schedule.state->Resolve(0, point), RHISubmissionPointStatus::eFailed);
}

TEST_P(RHIScheduledSubmissionTest, StandaloneRejectionCanRetryWithoutAdvancingFrame)
{
    const SmallVector<RHICommandContextType, 2> queues{RHICommandContextType::eAsyncCompute,
                                                       RHICommandContextType::eGraphics};
    TestOwnedSchedule rejected(*executor, queues);
    rejected.lists[0]->Dispatch(1, 1, 1);
    rejected.lists[1]->Dispatch(1, 1, 1);
    rejected.Depends(1, 0);
    rhi->failSubmissionAt = 1;
    EXPECT_EQ(executor->SubmitGroups(rejected.groups, rejected.state).submission,
              RHISubmissionResult::eRejected);
    EXPECT_FALSE(executor->AreSubmissionsBlocked());
    TestOwnedSchedule retry(*executor, queues);
    retry.lists[0]->Dispatch(1, 1, 1);
    retry.lists[1]->Dispatch(1, 1, 1);
    retry.Depends(1, 0);
    EXPECT_EQ(executor->SubmitGroups(retry.groups, retry.state).submission,
              RHISubmissionResult::eSuccess);
    EXPECT_EQ(ToValue(GetRHIThread().Invoke(&RHIFrameState::GetFrameNumber, &GRHIFrameState)), 0u);
}

TEST_P(RHIScheduledSubmissionTest, ComputeWithoutGraphicsJoinProtectsRetirementAndRecycling)
{
    const SmallVector<RHICommandContextType, 2> queues{RHICommandContextType::eGraphics,
                                                       RHICommandContextType::eAsyncCompute};
    RHIResourcePtr<RHIBuffer> resource = Buffer();
    const uint64_t id                  = resource->GetStableId();
    TestOwnedSchedule first(*executor, queues);
    first.lists[0]->Dispatch(1, 1, 1);
    first.lists[1]->ClearBuffer(resource.Get(), 0, 64);
    const RHIBatchResult submitted =
        executor->SubmitFrame(first.groups, nullptr, first.state).Wait();
    ASSERT_EQ(submitted.submission, RHISubmissionResult::eSuccess);
    resource.Reset();
    GetRHIThread().Invoke([this] { rhi->completed[0] = rhi->submitted[0]; });
    executor->FlushRHIThread();
    EXPECT_FALSE(destroyed.contains(id));
    EXPECT_TRUE(rhi->gpuDependencies.empty());
    GetRHIThread().Invoke([this] { rhi->completed[1] = rhi->submitted[1]; });
    executor->FlushRHIThread();
    EXPECT_TRUE(destroyed.contains(id));
    const SmallVector<RHICommandContextType, 3> reordered{RHICommandContextType::eTransfer,
                                                          RHICommandContextType::eAsyncCompute,
                                                          RHICommandContextType::eGraphics};
    TestOwnedSchedule next(*executor, reordered);
    for (RHICommandListPtr& list : next.lists)
    {
        list->Dispatch(1, 1, 1);
    }
    const RHIBatchResult recycled = executor->SubmitFrame(next.groups, nullptr, next.state).Wait();
    ASSERT_EQ(recycled.submission, RHISubmissionResult::eSuccess);
    ASSERT_EQ(recycled.groups.size(), 3u);
    for (size_t i = 0; i < reordered.size(); ++i)
    {
        EXPECT_EQ(recycled.groups[i].accepted.queue, reordered[i]);
        EXPECT_EQ(next.lists[i]->GetContext()->GetContextType(), reordered[i]);
    }
}

INSTANTIATE_TEST_SUITE_P(InlineThreadedAndAliases,
                         RHIScheduledSubmissionTest,
                         testing::Combine(testing::Values(RHIExecutionMode::eInline,
                                                          RHIExecutionMode::eThreaded),
                                          testing::Bool()));

class RDGScheduledSubmissionTest : public RDGQueuePreferenceTest
{};

TEST_P(RDGScheduledSubmissionTest, FrameSubmitsIndependentPrefixComputeAndConsumerOnce)
{
    RenderGraph& graph = *device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph.Begin());
    const RDGBuffer output = graph.GetResourceManager()->CreateBuffer(LogicalBuffer());
    graph.AddComputePass(IntentPass("prefix")).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
        encoder.Dispatch(1, 1, 1);
    });
    RDGComputePassDesc compute = IntentPass("producer");
    compute.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
    compute.BindStorageBuffer("write_buffer", output, RDGContentGuarantee::eFullWrite);
    graph.AddComputePass(compute).RecordPassCommands(
        [](RDGPassCmdEncoder& encoder) { encoder.Dispatch(1, 1, 1); });
    AddScheduledBufferPass(graph, "consumer", RDGQueue::eGraphics, output);
    RDGExtractedBuffer extracted = graph.GetResourceManager()->QueueBufferExtraction(output);
    ASSERT_TRUE(graph.End());
    const uint64_t batches = device->GetRHIThreadMetrics().submittedBatches;
    ASSERT_TRUE(device->ExecuteRenderGraph(&viewport));
    device->FlushRHIThread();
    EXPECT_TRUE(extracted);
    EXPECT_EQ(rhi->graphics.dispatchCount, 1u);
    EXPECT_EQ(rhi->compute.dispatchCount, 1u);
    EXPECT_EQ(viewport.presents, 1u);
    EXPECT_EQ(device->GetRHIThreadMetrics().submittedBatches, batches + 1);
    ASSERT_EQ(rhi->gpuDependencies.size(), 1u);
    EXPECT_EQ(rhi->gpuDependencies[0].consumer, RHICommandContextType::eGraphics);
    EXPECT_EQ(rhi->gpuDependencies[0].producer.queue, RHICommandContextType::eAsyncCompute);
    EXPECT_EQ(rhi->gpuDependencies[0].producer.serial, 1u);
    EXPECT_TRUE(rhi->submissionWaits.empty());
    device->NextFrame();
    ASSERT_TRUE(graph.Begin());
    graph.AddComputePass(IntentPass("cached_graphics"))
        .RecordPassCommands([](RDGPassCmdEncoder& encoder) { encoder.Dispatch(1, 1, 1); });
    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(&viewport));
    device->FlushRHIThread();
    EXPECT_EQ(rhi->submitted[1], 1u); // No empty compute submission on an ordinary frame.
    EXPECT_EQ(viewport.presents, 2u);
    extracted.Reset();
}

TEST_P(RDGScheduledSubmissionTest, LaterRecordingFailurePublishesNoGroupsOrExtraction)
{
    RenderGraph graph("recording_rollback");
    ASSERT_TRUE(graph.Begin());
    const RDGBuffer output      = graph.GetResourceManager()->CreateBuffer(LogicalBuffer());
    RDGComputePassDesc producer = IntentPass();
    producer.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
    producer.BindStorageBuffer("write_buffer", output, RDGContentGuarantee::eFullWrite);
    graph.AddComputePass(producer).RecordPassCommands(
        [](RDGPassCmdEncoder& encoder) { encoder.Dispatch(1, 1, 1); });
    RDGComputePassDesc failed = IntentPass();
    failed.BindStorageBuffer("read_buffer", output);
    graph.AddComputePass(failed).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
        encoder.Fail(RDGErrorCode::eCallback, "later group failed");
    });
    RDGExtractedBuffer extracted = graph.GetResourceManager()->QueueBufferExtraction(output);
    ASSERT_TRUE(graph.End());
    EXPECT_FALSE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(rhi->submissionAttempts, 0u);
    EXPECT_FALSE(extracted);
    EXPECT_FALSE(device->AreSubmissionsBlocked());
}

INSTANTIATE_TEST_SUITE_P(InlineAndThreaded,
                         RDGScheduledSubmissionTest,
                         testing::Values(RHIExecutionMode::eInline, RHIExecutionMode::eThreaded));

class ThreadedScheduledGraphTest : public ThreadedRenderCoreTest
{
protected:
    void SetUp() override
    {
        InitializeDevice(&viewport, 3, RHIExecutionMode::eThreaded, true, AsyncComputeMode::eAuto,
                         DistinctComputeQueues());
        CreateTestShaderProgram(device, "intent");
    }
};

TEST_F(ThreadedScheduledGraphTest, NextFrameRecordsAfterGraphResetWhileOwnedProducerIsPending)
{
    TestBuffer* buffer = Buffer();
    RenderGraph& graph = *device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph.Begin());
    const RDGBuffer output      = graph.GetResourceManager()->ImportBuffer(buffer);
    RDGComputePassDesc producer = IntentPass();
    producer.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
    producer.BindStorageBuffer("write_buffer", output, RDGContentGuarantee::eFullWrite);
    graph.AddComputePass(producer).RecordPassCommands(
        [](RDGPassCmdEncoder& encoder) { encoder.Dispatch(1, 1, 1); });
    RDGExtractedBuffer extracted = graph.GetResourceManager()->QueueBufferExtraction(output);
    ASSERT_TRUE(graph.End());
    ArmGate();
    ASSERT_TRUE(device->ExecuteRenderGraph(&viewport));
    ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_FALSE(extracted);
    device->NextFrame();
    ASSERT_TRUE(graph.Begin());
    // Reuse the already warm compute pipeline, but record this consumer on graphics.
    producer = IntentPass();
    producer.BindStorageBuffer("write_buffer", buffer, RDGContentGuarantee::eFullWrite);
    const std::thread::id renderThread = std::this_thread::get_id();
    bool recorded                      = false;
    graph.AddComputePass(producer).RecordPassCommands(
        [this, &recorded, renderThread](RDGPassCmdEncoder& encoder) {
            EXPECT_EQ(std::this_thread::get_id(), renderThread);
            EXPECT_FALSE(gate.releasedInTime);
            recorded = true;
            encoder.Dispatch(1, 1, 1);
            gate.Open();
        });
    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(&viewport));
    device->FlushRHIThread();
    EXPECT_TRUE(recorded);
    EXPECT_TRUE(extracted);
    EXPECT_EQ(rhi->compute.dispatchCount, 1u);
    EXPECT_EQ(rhi->graphics.dispatchCount, 1u);
    ASSERT_EQ(rhi->gpuDependencies.size(), 2u); // First extraction and later overwrite.
    for (const TestRHI::SubmissionDependency& wait : rhi->gpuDependencies)
    {
        EXPECT_EQ(wait.producer.queue, RHICommandContextType::eAsyncCompute);
        EXPECT_EQ(wait.producer.serial, 1u);
    }
    EXPECT_TRUE(rhi->submissionWaits.empty());
    extracted.Reset();
    device->DestroyBuffer(buffer);
}
} // namespace
