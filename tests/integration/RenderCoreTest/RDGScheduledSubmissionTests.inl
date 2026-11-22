namespace
{
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
    graph.AddComputePass(compute).RecordPassCommands([](RDGPassCmdEncoder& encoder) { encoder.Dispatch(1, 1, 1); });
    AddScheduledBufferPass(graph, "consumer", RHICommandContextType::eGraphics, output);
    RDGExtractedBuffer extracted = graph.GetResourceManager()->QueueBufferExtraction(output);
    ASSERT_TRUE(graph.End());
    const uint64_t batches = device->GetRHIThreadMetrics().submittedBatches;
    ASSERT_EQ(device->ExecuteRenderGraph(&viewport), RenderFrameResult::eSubmitted);
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
    graph.AddComputePass(IntentPass("cached_graphics")).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
        encoder.Dispatch(1, 1, 1);
    });
    ASSERT_TRUE(graph.End());
    ASSERT_EQ(device->ExecuteRenderGraph(&viewport), RenderFrameResult::eSubmitted);
    device->FlushRHIThread();
    EXPECT_EQ(rhi->submitted[1], 1u); // No empty compute submission on an ordinary frame.
    EXPECT_EQ(viewport.presents, 2u);
    extracted.Reset();
}

TEST_P(RDGScheduledSubmissionTest, LaterRecordingFailurePublishesNoGroupsOrExtraction)
{
    RenderGraph graph("recording_rollback");
    ASSERT_TRUE(graph.Begin());
    const RDGBuffer    output   = graph.GetResourceManager()->CreateBuffer(LogicalBuffer());
    RDGComputePassDesc producer = IntentPass();
    producer.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
    producer.BindStorageBuffer("write_buffer", output, RDGContentGuarantee::eFullWrite);
    graph.AddComputePass(producer).RecordPassCommands([](RDGPassCmdEncoder& encoder) { encoder.Dispatch(1, 1, 1); });
    RDGComputePassDesc failed = IntentPass();
    failed.BindStorageBuffer("read_buffer", output);
    graph.AddComputePass(failed).RecordPassCommands(
        [](RDGPassCmdEncoder& encoder) { encoder.Fail(RDGErrorCode::eCallback, "later group failed"); });
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
        InitializeDevice(&viewport, 3, RHIExecutionMode::eThreaded, AsyncComputeMode::eAuto, DistinctComputeQueues());
        CreateTestShaderProgram(device, "intent");
    }
};

TEST_F(ThreadedScheduledGraphTest, NextFrameRecordsAfterGraphResetWhileOwnedProducerIsPending)
{
    TestBuffer*  buffer = Buffer();
    RenderGraph& graph  = *device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph.Begin());
    const RDGBuffer    output   = graph.GetResourceManager()->ImportBuffer(buffer);
    RDGComputePassDesc producer = IntentPass();
    producer.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
    producer.BindStorageBuffer("write_buffer", output, RDGContentGuarantee::eFullWrite);
    graph.AddComputePass(producer).RecordPassCommands([](RDGPassCmdEncoder& encoder) { encoder.Dispatch(1, 1, 1); });
    RDGExtractedBuffer extracted = graph.GetResourceManager()->QueueBufferExtraction(output);
    ASSERT_TRUE(graph.End());
    ArmGate();
    ASSERT_EQ(device->ExecuteRenderGraph(&viewport), RenderFrameResult::eSubmitted);
    ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_FALSE(extracted);
    device->NextFrame();
    ASSERT_TRUE(graph.Begin());
    // Reuse the already warm compute pipeline, but record this consumer on graphics.
    producer = IntentPass();
    producer.BindStorageBuffer("write_buffer", buffer, RDGContentGuarantee::eFullWrite);
    const std::thread::id renderThread = std::this_thread::get_id();
    bool                  recorded     = false;
    graph.AddComputePass(producer).RecordPassCommands([this, &recorded, renderThread](RDGPassCmdEncoder& encoder) {
        EXPECT_EQ(std::this_thread::get_id(), renderThread);
        EXPECT_FALSE(gate.releasedInTime);
        recorded = true;
        encoder.Dispatch(1, 1, 1);
        gate.Open();
    });
    ASSERT_TRUE(graph.End());
    ASSERT_EQ(device->ExecuteRenderGraph(&viewport), RenderFrameResult::eSubmitted);
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
