// Reuse the owned-schedule and RenderDevice fixtures from Step 7.
namespace
{
TEST_P(RHIScheduledSubmissionTest, PresentationRejectionAfterComputeIsFatalAndRetainsOwners)
{
    const SmallVector<RHICommandContextType, 1> queues{RHICommandContextType::eAsyncCompute};
    TestOwnedSchedule schedule(*executor, queues);
    RHIResourcePtr<RHIBuffer> resource = Buffer();
    const uint64_t id                  = resource->GetStableId();
    schedule.lists[0]->ClearBuffer(resource.Get(), 0, 64);
    viewport.recordPresentCommands = true;
    rhi->failSubmissionAt          = 2;
    const RHIBatchResult result =
        executor->SubmitFrame(schedule.groups, &viewport, schedule.state).Wait();
    EXPECT_EQ(result.submission, RHISubmissionResult::eFatal);
    EXPECT_EQ(result.groups[0].submission, RHISubmissionResult::eSuccess);
    EXPECT_EQ(result.groups[0].accepted.serial, 1u);
    EXPECT_EQ(result.completion.Get(queues[0]), 1u);
    EXPECT_EQ(viewport.preparePresents, 1u);
    EXPECT_EQ(viewport.presents, 0u);
    EXPECT_TRUE(executor->AreSubmissionsBlocked());
    EXPECT_FALSE(schedule.state->IsComplete());
    resource.Reset();
    executor->WaitDeviceIdle();
    EXPECT_FALSE(destroyed.contains(id));
    EXPECT_FALSE(executor->SubmitFrame(schedule.groups, nullptr).IsValid());
    executor->Destroy();
    EXPECT_TRUE(destroyed.contains(id));
}

TEST_P(RHIScheduledSubmissionTest, FirstGroupWithAcceptedWorkCannotBeRetried)
{
    const SmallVector<RHICommandContextType, 2> queues{RHICommandContextType::eAsyncCompute,
                                                       RHICommandContextType::eGraphics};
    TestOwnedSchedule schedule(*executor, queues);
    schedule.lists[0]->Dispatch(1, 1, 1);
    schedule.lists[1]->Dispatch(1, 1, 1);
    schedule.Depends(1, 0);
    rhi->failSubmissionAt       = 1;
    rhi->submitBeforeFailure    = true;
    const RHIBatchResult result = executor->SubmitGroups(schedule.groups, schedule.state);
    EXPECT_EQ(result.submission, RHISubmissionResult::eFatal);
    EXPECT_EQ(result.completion.Get(queues[0]), 1u);
    EXPECT_EQ(result.groups[0].accepted.serial, 1u);
    EXPECT_EQ(result.groups[1].submission, RHISubmissionResult::eRejected);
    EXPECT_EQ(rhi->submissionAttempts, 1u);
    EXPECT_EQ(rhi->graphics.dispatchCount, 0u);
    EXPECT_TRUE(executor->AreSubmissionsBlocked());
}

TEST_P(RHIScheduledSubmissionTest, CompletionQueryFailurePreservesAcceptedComputeSerial)
{
    const SmallVector<RHICommandContextType, 2> queues{RHICommandContextType::eAsyncCompute,
                                                       RHICommandContextType::eGraphics};
    TestOwnedSchedule schedule(*executor, queues);
    schedule.lists[0]->Dispatch(1, 1, 1);
    schedule.lists[1]->Dispatch(1, 1, 1);
    schedule.Depends(1, 0);
    rhi->beforeSubmission = [this] {
        rhi->failProgressQuery = true;
    };
    const RHIBatchResult result =
        executor->SubmitFrame(schedule.groups, &viewport, schedule.state).Wait();
    GetRHIThread().Invoke([this] { rhi->failProgressQuery = false; });
    EXPECT_EQ(result.submission, RHISubmissionResult::eFatal);
    EXPECT_EQ(result.completion.Get(queues[0]), 1u);
    EXPECT_EQ(result.groups[0].submission, RHISubmissionResult::eFatal);
    EXPECT_EQ(result.groups[1].submission, RHISubmissionResult::eRejected);
    EXPECT_EQ(rhi->submissionAttempts, 1u);
    EXPECT_EQ(viewport.presents, 0u);
    EXPECT_TRUE(executor->AreSubmissionsBlocked());
}

TEST_P(RHIScheduledSubmissionTest, FirstFrameGroupRejectionBlocksSubsequentFrames)
{
    const SmallVector<RHICommandContextType, 2> queues{RHICommandContextType::eAsyncCompute,
                                                       RHICommandContextType::eGraphics};
    TestOwnedSchedule schedule(*executor, queues);
    schedule.lists[0]->Dispatch(1, 1, 1);
    schedule.lists[1]->Dispatch(1, 1, 1);
    schedule.Depends(1, 0);
    rhi->failSubmissionAt = 1;
    const RHIBatchResult result =
        executor->SubmitFrame(schedule.groups, &viewport, schedule.state).Wait();
    EXPECT_EQ(result.submission, RHISubmissionResult::eRejected);
    EXPECT_EQ(result.completion.Get(queues[0]), 0u);
    EXPECT_EQ(result.groups[1].submission, RHISubmissionResult::eRejected);
    EXPECT_EQ(rhi->submissionAttempts, 1u);
    EXPECT_EQ(viewport.preparePresents, 0u);
    EXPECT_TRUE(executor->AreSubmissionsBlocked());
    EXPECT_FALSE(executor->SubmitFrame(schedule.groups, &viewport).IsValid());
}

RDGExtractedBuffer BuildFailureGraph(RenderGraph& graph, RHIBuffer* buffer)
{
    EXPECT_TRUE(graph.Begin());
    const RDGBuffer output      = graph.GetResourceManager()->ImportBuffer(buffer);
    RDGComputePassDesc producer = IntentPass();
    producer.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
    producer.BindStorageBuffer("write_buffer", output, RDGContentGuarantee::eFullWrite);
    graph.AddComputePass(producer).RecordPassCommands(
        [](RDGPassCmdEncoder& encoder) { encoder.Dispatch(1, 1, 1); });
    // Extraction contributes a dependent graphics group.
    RDGExtractedBuffer extracted = graph.GetResourceManager()->QueueBufferExtraction(output);
    EXPECT_TRUE(graph.End());
    return extracted;
}

TEST_P(RDGScheduledSubmissionTest, PartialFailureConsumesTicketAndKeepsAcceptedFrameRetirement)
{
    TestBuffer* buffer           = Buffer();
    RenderGraph& graph           = *device->GetCurrentFrameRDG();
    RDGExtractedBuffer extracted = BuildFailureGraph(graph, buffer);
    rhi->failSubmissionAt        = 2;
    const bool queued            = device->ExecuteRenderGraph(&viewport);
    if (GetParam() == RHIExecutionMode::eInline)
    {
        EXPECT_FALSE(queued);
    }
    device->FlushRHIThread();
    EXPECT_FALSE(extracted);
    EXPECT_TRUE(device->AreSubmissionsBlocked());
    EXPECT_EQ(RDGSubmissionTestAccess::PendingFrameCount(*device), 0u);
    EXPECT_EQ(
        RDGSubmissionTestAccess::FrameCompletion(*device).Get(RHICommandContextType::eAsyncCompute),
        1u);
    EXPECT_EQ(RDGSubmissionTestAccess::History(*device).Find(buffer->GetStableId()), nullptr);
    EXPECT_FALSE(RDGSubmissionTestAccess::HasHistory(RDGSubmissionTestAccess::Tracker(*device),
                                                     buffer->GetStableId()));
    EXPECT_FALSE(device->ExecuteRenderGraph(&viewport));
    EXPECT_EQ(rhi->submissionAttempts, 2u);
    EXPECT_EQ(viewport.preparePresents, 0u);
    device->DestroyBuffer(buffer);
}

TEST_P(RDGScheduledSubmissionTest, RecreationPreservesPublicationAndComputeRetirement)
{
    TestBuffer* buffer           = Buffer();
    RenderGraph& graph           = *device->GetCurrentFrameRDG();
    RDGExtractedBuffer extracted = BuildFailureGraph(graph, buffer);
    viewport.presentResult       = false;
    viewport.recreationRequested = true;
    const bool queued            = device->ExecuteRenderGraph(&viewport);
    if (GetParam() == RHIExecutionMode::eInline)
    {
        EXPECT_FALSE(queued);
    }
    device->FlushRHIThread();
    ASSERT_TRUE(extracted);
    EXPECT_FALSE(device->AreSubmissionsBlocked());
    EXPECT_EQ(RDGSubmissionTestAccess::RecreateViewport(*device), &viewport);
    EXPECT_EQ(RDGSubmissionTestAccess::Submission(*device, buffer).serial, 1u);
    EXPECT_EQ(
        RDGSubmissionTestAccess::FrameCompletion(*device).Get(RHICommandContextType::eAsyncCompute),
        1u);
    EXPECT_EQ(rhi->completed[1], 0u); // Publication does not wait for GPU completion.
    device->NextFrame();
    EXPECT_EQ(viewport.resizes, 1u);
    EXPECT_EQ(RDGSubmissionTestAccess::RecreateViewport(*device), nullptr);
    EXPECT_GE(rhi->completed[1], 1u);
    EXPECT_TRUE(extracted);
    extracted.Reset();
    device->DestroyBuffer(buffer);
}

TEST_P(RDGScheduledSubmissionTest, DeclinedHandoffRetainsPriorHistoryAndAllowsRetry)
{
    TestBuffer* buffer         = Buffer();
    RenderGraph& graph         = *device->GetCurrentFrameRDG();
    RDGExtractedBuffer initial = BuildFailureGraph(graph, buffer);
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    const RHISubmissionDependency previous = RDGSubmissionTestAccess::Submission(*device, buffer);
    const uint64_t attempts                = rhi->submissionAttempts;
    RDGExtractedBuffer declined            = BuildFailureGraph(graph, buffer);
    RDGSubmissionTestAccess::SetRecreateViewport(*device, &viewport);
    EXPECT_FALSE(device->ExecuteRenderGraph(&viewport));
    EXPECT_FALSE(declined);
    EXPECT_TRUE(initial);
    EXPECT_EQ(rhi->submissionAttempts, attempts);
    EXPECT_EQ(RDGSubmissionTestAccess::PendingFrameCount(*device), 0u);
    EXPECT_FALSE(device->AreSubmissionsBlocked());
    const RHISubmissionDependency preserved = RDGSubmissionTestAccess::Submission(*device, buffer);
    EXPECT_EQ(preserved.queue, previous.queue);
    EXPECT_EQ(preserved.serial, previous.serial);
    EXPECT_EQ(RDGSubmissionTestAccess::Tracker(*device).GetContents(buffer).status,
              RDGContentStatus::eDefined);
    RDGSubmissionTestAccess::SetRecreateViewport(*device, nullptr);
    RDGExtractedBuffer retry = BuildFailureGraph(graph, buffer);
    ASSERT_TRUE(device->ExecuteRenderGraph(&viewport));
    device->FlushRHIThread();
    EXPECT_TRUE(retry);
    EXPECT_EQ(rhi->submitted[1], previous.serial + 1);
    initial.Reset();
    retry.Reset();
    device->DestroyBuffer(buffer);
}

TEST_P(RDGScheduledSubmissionTest, ImmediateHandoffFailureCanRestoreConsumedVoxelRequest)
{
    TestVoxelVolumes volumes(device, DataFormat::eR8G8B8A8UNORM);
    volumes.Init();
    RenderGraph& graph = *device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph.Begin());
    EXPECT_TRUE(volumes.BeginVolumeUpdate(graph));
    EXPECT_FALSE(volumes.BeginVolumeUpdate(graph));
    ASSERT_TRUE(graph.End());
    RDGSubmissionTestAccess::SetRecreateViewport(*device, &viewport);
    const bool succeeded = device->ExecuteRenderGraph(&viewport);
    EXPECT_FALSE(succeeded);
    // RendererServer's immediate-failure path restores the consumed request.
    if (!succeeded)
    {
        volumes.RequestVoxelization();
    }
    RDGSubmissionTestAccess::SetRecreateViewport(*device, nullptr);
    ASSERT_TRUE(graph.Begin());
    EXPECT_TRUE(volumes.BeginVolumeUpdate(graph));
    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(&viewport));
    device->FlushRHIThread();
    EXPECT_EQ(rhi->graphics.textureClears.size(), 1u);
    ASSERT_TRUE(graph.Begin());
    EXPECT_FALSE(volumes.BeginVolumeUpdate(graph));
    volumes.Destroy();
}

TEST_F(ThreadedScheduledGraphTest, FailedProducerDiscardsAlreadyRecordedSpeculativeFrame)
{
    TestBuffer* buffer       = Buffer();
    RenderGraph& graph       = *device->GetCurrentFrameRDG();
    RDGExtractedBuffer first = BuildFailureGraph(graph, buffer);
    rhi->failSubmissionAt    = 2;
    ArmGate();
    ASSERT_TRUE(device->ExecuteRenderGraph(&viewport));
    ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    device->NextFrame();
    ASSERT_TRUE(graph.Begin());
    const RDGBuffer output      = graph.GetResourceManager()->ImportBuffer(buffer);
    RDGComputePassDesc consumer = IntentPass();
    consumer.BindStorageBuffer("write_buffer", output, RDGContentGuarantee::eFullWrite);
    bool recorded = false;
    graph.AddComputePass(consumer).RecordPassCommands(
        [this, &recorded](RDGPassCmdEncoder& encoder) {
            recorded = true;
            encoder.Dispatch(1, 1, 1);
            gate.Open();
        });
    RDGExtractedBuffer second = graph.GetResourceManager()->QueueBufferExtraction(output);
    ASSERT_TRUE(graph.End());
    EXPECT_FALSE(device->ExecuteRenderGraph(&viewport));
    device->FlushRHIThread();
    EXPECT_TRUE(recorded);
    EXPECT_TRUE(gate.releasedInTime);
    EXPECT_FALSE(first);
    EXPECT_FALSE(second);
    EXPECT_TRUE(device->AreSubmissionsBlocked());
    EXPECT_EQ(rhi->submissionAttempts, 2u);
    EXPECT_EQ(device->GetRHIThreadMetrics().submittedBatches, 1u);
    EXPECT_EQ(RDGSubmissionTestAccess::History(*device).Find(buffer->GetStableId()), nullptr);
    EXPECT_FALSE(RDGSubmissionTestAccess::HasHistory(RDGSubmissionTestAccess::Tracker(*device),
                                                     buffer->GetStableId()));
    device->DestroyBuffer(buffer);
}

TEST_F(ThreadedScheduledGraphTest, UnconfirmedHistoryCannotPublishSuccessfulNativeFrame)
{
    TestBuffer* buffer           = Buffer();
    RenderGraph& graph           = *device->GetCurrentFrameRDG();
    RDGExtractedBuffer extracted = BuildFailureGraph(graph, buffer);
    ArmGate();
    ASSERT_TRUE(device->ExecuteRenderGraph(&viewport));
    ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const RenderResourceHistory* history =
        RDGSubmissionTestAccess::History(*device).Find(buffer->GetStableId());
    ASSERT_NE(history, nullptr);
    RefCountPtr<RHISubmissionState> state = history->writer.point.state;
    ASSERT_TRUE(state);
    gate.Open();
    GetRHIThread().Flush();
    ASSERT_TRUE(state->IsComplete());
    // Inject invalid confirmation after native success, before RenderCore publication.
    state->Fail();
    device->FlushRHIThread();
    EXPECT_TRUE(device->AreSubmissionsBlocked());
    EXPECT_FALSE(extracted);
    EXPECT_EQ(RDGSubmissionTestAccess::History(*device).Find(buffer->GetStableId()), nullptr);
    EXPECT_EQ(
        RDGSubmissionTestAccess::FrameCompletion(*device).Get(RHICommandContextType::eAsyncCompute),
        1u);
    device->DestroyBuffer(buffer);
}
} // namespace
