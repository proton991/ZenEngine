namespace
{
class VoxelAsyncResetTest :
    public RenderCoreTest,
    public testing::WithParamInterface<std::tuple<RHIExecutionMode, bool>>
{
protected:
    void SetUp() override
    {
        const RHIQueueCopyCapabilities compute{false, std::get<1>(GetParam()), true, {1, 1, 1}};
        InitializeDevice(nullptr, 2, std::get<0>(GetParam()), true, AsyncComputeMode::eAuto,
                         DistinctComputeQueues(), compute);
        CaptureVersionGraph(device);
        RDGMetricsOptions options    = device->GetRDGMetrics().GetOptions();
        options.includeTransferNodes = true;
        device->GetRDGMetrics().Configure(options);
    }
};

TEST_P(VoxelAsyncResetTest, PreferenceReachesResetAndUnsupportedClearFallsBack)
{
    TestVoxelVolumes volumes(device, DataFormat::eR8G8B8A8UNORM);
    volumes.Init();
    for (RDGQueuePreference preference :
         {RDGQueuePreference::eDefault, RDGQueuePreference::ePreferAsyncCompute})
    {
        RenderGraph graph("voxel_reset_preference");
        ASSERT_TRUE(graph.Begin());
        volumes.RequestVoxelization();
        EXPECT_TRUE(volumes.BeginVolumeUpdate(graph, preference));
        EXPECT_FALSE(volumes.BeginVolumeUpdate(graph, preference));
        ASSERT_TRUE(graph.End());
        ASSERT_TRUE(device->ExecuteRenderGraph(graph));
        const bool compute =
            preference == RDGQueuePreference::ePreferAsyncCompute && std::get<1>(GetParam());
        const RDGMetricsSnapshot& capture = device->GetRDGMetrics().GetLastSnapshot();
        ASSERT_EQ(capture.nodes.size(), 1u);
        EXPECT_EQ(capture.nodes[0].name, NameID("ResetVoxelVolumes"));
        EXPECT_EQ(capture.nodes[0].queuePreference, preference);
        EXPECT_EQ(capture.nodes[0].plannedQueue,
                  compute ? RDGQueue::eAsyncCompute : RDGQueue::eGraphics);
        EXPECT_EQ(RDGSubmissionTestAccess::LoggedAsyncCompute(*device), compute);
    }
    EXPECT_EQ(rhi->compute.textureClears.size(), std::get<1>(GetParam()) ? 1u : 0u);
    EXPECT_EQ(rhi->graphics.textureClears.size(), std::get<1>(GetParam()) ? 1u : 2u);
    volumes.Destroy();
}

INSTANTIATE_TEST_SUITE_P(InlineThreadedAndClearSupport,
                         VoxelAsyncResetTest,
                         testing::Combine(testing::Values(RHIExecutionMode::eInline,
                                                          RHIExecutionMode::eThreaded),
                                          testing::Bool()));

TEST_P(RDGScheduledSubmissionTest, CapturesAcceptedComputeAndExactCrossFrameDependencies)
{
    TestBuffer* buffer       = Buffer();
    RenderGraph& graph       = *device->GetCurrentFrameRDG();
    RDGExtractedBuffer first = BuildFailureGraph(graph, buffer);
    EXPECT_FALSE(RDGSubmissionTestAccess::LoggedAsyncCompute(*device));
    ASSERT_TRUE(device->ExecuteRenderGraph(&viewport));
    device->FlushRHIThread();
    ASSERT_TRUE(first);
    EXPECT_TRUE(RDGSubmissionTestAccess::LoggedAsyncCompute(*device));
    const RDGMetricsSnapshot initial = device->GetRDGMetrics().GetLastSnapshot();
    ASSERT_EQ(initial.submissions.size(), 2u);
    EXPECT_EQ(initial.submissions[0].queue, RDGQueue::eAsyncCompute);
    EXPECT_EQ(initial.submissions[0].queueEquivalenceId, 1u);
    ASSERT_EQ(initial.submissions[1].dependencies.size(), 1u);
    EXPECT_EQ(initial.submissions[1].dependencies[0].producer, 0u);
    EXPECT_TRUE(initial.submissions[1].dependencies[0].semaphore);
    EXPECT_EQ(initial.submissions[1].dependencies[0].producerQueue, RDGQueue::eAsyncCompute);
    EXPECT_FALSE(initial.submissions[1].dependencies[0].external);
    EXPECT_EQ(initial.submissions[1].waitStages, int64_t(RHIPipelineStageFlagBits::eAllCommands));
    device->NextFrame();
    RDGExtractedBuffer second = BuildFailureGraph(graph, buffer);
    ASSERT_TRUE(device->ExecuteRenderGraph(&viewport));
    device->FlushRHIThread();
    ASSERT_TRUE(second);
    const RDGMetricsSnapshot& repeated = device->GetRDGMetrics().GetLastSnapshot();
    bool foundExternalSemaphore        = false;
    for (const RDGSubmissionDependencyMetrics& dependency : repeated.submissions[0].dependencies)
    {
        foundExternalSemaphore |= dependency.external && dependency.semaphore &&
            dependency.producerQueue == RDGQueue::eGraphics;
    }
    EXPECT_TRUE(foundExternalSemaphore);
    EXPECT_GE(repeated.submissionCPUUs, 0.0);
    const std::string formatted = RDGMetrics::Format(repeated);
    EXPECT_NE(formatted.find("native_queue_id=1"), std::string::npos);
    EXPECT_NE(formatted.find("producer_external="), std::string::npos);
    EXPECT_NE(formatted.find("synchronization=semaphore_boundary"), std::string::npos);
    first.Reset();
    second.Reset();
    device->DestroyBuffer(buffer);
}

TEST_P(RDGScheduledSubmissionTest, CaptureLimitsDoNotChangeSubmissionDependencies)
{
    RDGMetricsOptions options    = device->GetRDGMetrics().GetOptions();
    options.maxSubmissionDetails = 1;
    options.maxDependencyDetails = 0;
    device->GetRDGMetrics().Configure(options);
    TestBuffer* buffer        = Buffer();
    RenderGraph& graph        = *device->GetCurrentFrameRDG();
    RDGExtractedBuffer output = BuildFailureGraph(graph, buffer);
    ASSERT_TRUE(device->ExecuteRenderGraph(&viewport));
    device->FlushRHIThread();
    ASSERT_TRUE(output);
    const RDGMetricsSnapshot& capture = device->GetRDGMetrics().GetLastSnapshot();
    EXPECT_EQ(capture.submissions.size(), 1u);
    EXPECT_EQ(capture.omittedSubmissionDetails, 1u);
    EXPECT_EQ(capture.omittedDependencyDetails, 1u);
    EXPECT_TRUE(capture.submissions[0].dependencies.empty());
    ASSERT_EQ(rhi->gpuDependencies.size(), 1u);
    EXPECT_EQ(rhi->gpuDependencies[0].producer.queue, RHICommandContextType::eAsyncCompute);
    output.Reset();
    device->DestroyBuffer(buffer);
}

TEST_P(RDGScheduledSubmissionTest, FailedComputeFrameDoesNotReportFirstUse)
{
    TestBuffer* buffer        = Buffer();
    RDGExtractedBuffer output = BuildFailureGraph(*device->GetCurrentFrameRDG(), buffer);
    rhi->failSubmissionAt     = 2;
    device->ExecuteRenderGraph(&viewport);
    device->FlushRHIThread();
    EXPECT_TRUE(device->AreSubmissionsBlocked());
    EXPECT_FALSE(output);
    EXPECT_FALSE(RDGSubmissionTestAccess::LoggedAsyncCompute(*device));
    device->DestroyBuffer(buffer);
}
} // namespace
