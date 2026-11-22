namespace
{
class RDGDebugSnapshotTest : public RenderCoreTest, public testing::WithParamInterface<std::tuple<RHIExecutionMode, bool>>
{
protected:
    TestViewport viewport;

    void SetUp() override
    {
        InitializeDevice(&viewport, 2, std::get<0>(GetParam()),
                         std::get<1>(GetParam()) ? AsyncComputeMode::eAuto : AsyncComputeMode::eDisabled,
                         DistinctComputeQueues());
        device->GetRDGMetrics().Configure({});
    }

    void TearDown() override
    {
        viewport.ReleaseForTeardown();
        RenderCoreTest::TearDown();
    }
};

TEST_P(RDGDebugSnapshotTest, CompleteCaptureOwnsNamesAccessesAndCulledPassesAfterReset)
{
    RDGDebugSnapshot snapshot;
    RHITexture*      texture = Texture();
    RenderGraph&     graph   = *device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph.Begin());
    for (uint32_t i = 0; i < 40; ++i)
    {
        const std::string name = fmt::format("LivePass{}", i);
        graph.AddTransferPass(name.c_str())
            .NeverCull()
            .SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute)
            .ClearTexture(texture, Color(0.f));
    }
    const RDGTexture unused = graph.GetResourceManager()->CreateTexture(LogicalTexture());
    graph.AddTransferPass("Unused").ClearTexture(unused, Color(0.f));
    ASSERT_TRUE(graph.End());
    ASSERT_EQ(device->ExecuteRenderGraph(&viewport, &snapshot), RenderFrameResult::eSubmitted);
    ASSERT_TRUE(snapshot.accepted);
    ASSERT_EQ(snapshot.passes.size(), 41u);
    EXPECT_EQ(snapshot.mode, std::get<1>(GetParam()) ? RDGDebugExecutionMode::Grouped : RDGDebugExecutionMode::Graphics);
    EXPECT_FALSE(snapshot.passes[40].live);
    EXPECT_EQ(snapshot.passes[40].order, UINT32_MAX);
    EXPECT_FALSE(snapshot.passes[40].accesses.empty());
    EXPECT_FALSE(snapshot.dependencies.empty());
    for (const RDGDependency& edge : snapshot.dependencies)
    {
        EXPECT_NE(int32_t(edge.source), 40);
        EXPECT_NE(int32_t(edge.destination), 40);
    }
    device->NextFrame();
    ASSERT_TRUE(graph.Reset());
    EXPECT_EQ(snapshot.passes[39].name, "LivePass39");
    EXPECT_FALSE(snapshot.passes[39].versions.empty());
    EXPECT_EQ(snapshot.resources.size(), 2u);
    device->DestroyTexture(texture);
}

TEST_P(RDGDebugSnapshotTest, OrdinaryFrameCannotOverwriteCaptureAndInvalidFrameDoesNotPublish)
{
    RDGDebugSnapshot snapshot;
    RenderGraph&     graph = *device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph.Begin());
    graph.AddTransferPass("First").NeverCull();
    ASSERT_TRUE(graph.End());
    ASSERT_EQ(device->ExecuteRenderGraph(&viewport, &snapshot), RenderFrameResult::eSubmitted);
    device->NextFrame();
    ASSERT_TRUE(graph.Begin());
    graph.AddTransferPass("Popup").NeverCull();
    ASSERT_TRUE(graph.End());
    ASSERT_EQ(device->ExecuteRenderGraph(&viewport), RenderFrameResult::eSubmitted);
    EXPECT_EQ(snapshot.passes[0].name, "First");
    device->NextFrame();
    ASSERT_TRUE(graph.Begin());
    RDGDebugSnapshot failed;
    EXPECT_NE(device->ExecuteRenderGraph(&viewport, &failed), RenderFrameResult::eSubmitted);
    EXPECT_FALSE(failed.accepted);
    EXPECT_EQ(snapshot.passes[0].name, "First");
}

TEST_P(RDGDebugSnapshotTest, CaptureIncludesEachContentWarningOnceAndResourcesUseDenseIds)
{
    CreateTestShaderProgram(device, "intent");
    TestBuffer*  buffer = Buffer();
    RenderGraph& graph  = *device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph.Begin());
    RDGComputePassDesc read = IntentPass();
    read.BindStorageBuffer("read_buffer", buffer, RDGContentGuarantee::eNone);
    graph.AddComputePass(read);
    ASSERT_TRUE(graph.End());
    RDGDebugSnapshot snapshot;
    ASSERT_EQ(device->ExecuteRenderGraph(&viewport, &snapshot), RenderFrameResult::eSubmitted);
    ASSERT_EQ(graph.GetWarnings().size(), 1u);
    ASSERT_EQ(snapshot.warnings.size(), 1u);
    EXPECT_EQ(snapshot.warnings[0], graph.GetWarnings()[0].message);
    for (size_t id = 0; id < snapshot.resources.size(); ++id)
    {
        EXPECT_EQ(snapshot.resources[id].id, int32_t(id));
    }
    device->NextFrame();
    device->DestroyBuffer(buffer);
}

INSTANTIATE_TEST_SUITE_P(ExecutionModes,
                         RDGDebugSnapshotTest,
                         testing::Combine(testing::Values(RHIExecutionMode::eInline, RHIExecutionMode::eThreaded),
                                          testing::Bool()));
} // namespace
