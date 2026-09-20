// Included after the shared RenderCore fixtures and execution-plan test access.
namespace
{
RHIQueueCapabilities DistinctComputeQueues()
{
    return {true, true, {0, 1, 2}};
}

class RDGQueuePreferenceTest :
    public RenderCoreTest,
    public testing::WithParamInterface<RHIExecutionMode>
{
protected:
    TestViewport viewport;

    void SetUp() override
    {
        InitializeDevice(&viewport, 2, GetParam(), true, AsyncComputeMode::eAuto,
                         DistinctComputeQueues());
        CreateTestShaderProgram(device, "intent");
        CaptureVersionGraph(device);
    }
};

TEST_P(RDGQueuePreferenceTest, DescriptorCopiesCompiledDataAndMetricsPreserveHints)
{
    RenderGraph graph("queue_preferences");
    ASSERT_TRUE(graph.Begin());
    graph.AddComputePass(IntentPass("default_compute"));
    RDGComputePassDesc preferred = IntentPass("preferred_compute");
    preferred.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
    graph.AddComputePass(preferred);
    preferred.SetQueuePreference(RDGQueuePreference::eDefault);
    RDGGraphicsPassDesc graphics;
    graphics.SetShaderProgramName("intent");
    graphics.SetPassTag("graphics");
    graph.AddGraphicsPass(graphics);
    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    const RDGMetricsSnapshot& metrics = device->GetRDGMetrics().GetLastSnapshot();
    ASSERT_EQ(metrics.nodes.size(), 3u);
    SmallVector<const RDGNodeMetrics*, 3> nodes(3);
    for (const RDGNodeMetrics& node : metrics.nodes)
    {
        nodes[size_t(node.id)] = &node;
    }
    EXPECT_EQ(nodes[0]->queuePreference, RDGQueuePreference::eDefault);
    EXPECT_EQ(nodes[0]->asyncComputeEligibility, RDGAsyncComputeEligibility::eNotRequested);
    EXPECT_EQ(nodes[1]->queuePreference, RDGQueuePreference::ePreferAsyncCompute);
    EXPECT_EQ(nodes[1]->asyncComputeEligibility, RDGAsyncComputeEligibility::eEligible);
    EXPECT_EQ(nodes[2]->queuePreference, RDGQueuePreference::eDefault);
    EXPECT_EQ(nodes[2]->asyncComputeEligibility, RDGAsyncComputeEligibility::eGraphicsPass);
    const RDGPassNode& recorded = RDGExecutionPlanTestAccess::RecordedNode(graph, 1);
    EXPECT_EQ(recorded.queuePreference, RDGQueuePreference::ePreferAsyncCompute);
    EXPECT_EQ(recorded.pCompiledPass->queuePreference, recorded.queuePreference);
    EXPECT_NE(RDGMetrics::Format(metrics).find(
                  "queue_preference=prefer_async_compute async_eligibility=eligible"),
              std::string::npos);
    // Step 7 now submits eligible passes on the selected native compute queue.
    EXPECT_EQ(rhi->submitted[size_t(RHICommandContextType::eAsyncCompute)], 1u);
    EXPECT_GT(rhi->submitted[size_t(RHICommandContextType::eGraphics)], 0u);
}

TEST_P(RDGQueuePreferenceTest, ClearSupportsComputeButMipBlitsAccumulateGraphicsRequirement)
{
    for (bool logical : {false, true})
    {
        for (bool mipmaps : {false, true})
        {
            RenderGraph graph("transfer_requirements");
            ASSERT_TRUE(graph.Begin());
            RHITexture* physical     = logical ? nullptr : Texture(3);
            const RDGTexture texture = logical ?
                graph.GetResourceManager()->CreateTexture(LogicalTexture(3)) :
                RDGTexture{};
            {
                RDGTransferPassCmdRecorder recorder = graph.AddTransferPass("reset_volumes");
                recorder.NeverCull().SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
                if (logical)
                {
                    recorder.ClearTexture(texture, Color(0.f));
                    if (mipmaps)
                    {
                        recorder.GenerateMipmaps(texture).ClearTexture(texture, Color(1.f));
                    }
                }
                else
                {
                    recorder.ClearTexture(physical, Color(0.f));
                    if (mipmaps)
                    {
                        recorder.GenerateMipmaps(physical).ClearTexture(physical, Color(1.f));
                    }
                }
                // Setting the preference after commands must preserve accumulated restrictions.
                recorder.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
            }
            ASSERT_TRUE(graph.End());
            RDGExecutor executor(device);
            RDGExecutionPlanTestAccess::Plan plan;
            ASSERT_TRUE(RDGExecutionPlanTestAccess::Prepare(executor, graph, plan));
            EXPECT_FALSE(plan.transfer);
            const RDGCompiledNode& compiled = RDGExecutionPlanTestAccess::CompiledNode(graph);
            EXPECT_EQ(compiled.queuePreference, RDGQueuePreference::ePreferAsyncCompute);
            EXPECT_EQ(compiled.asyncComputeEligibility,
                      mipmaps ? RDGAsyncComputeEligibility::eUnsupportedCommands :
                                RDGAsyncComputeEligibility::eEligible);
            EXPECT_EQ(
                RDGExecutionPlanTestAccess::RecordedNode(graph).pCompiledPass->queuePreference,
                compiled.queuePreference);
            if (physical != nullptr)
            {
                device->DestroyTexture(physical);
            }
        }
    }
}

TEST_P(RDGQueuePreferenceTest, DefaultBufferCopyKeepsTransferRoutingAndPreferredCopyIsEligible)
{
    TestBuffer* source = Buffer();
    TestBuffer* output = Buffer();
    std::fill(source->bytes.begin(), source->bytes.end(), uint8_t(31));
    for (RDGQueuePreference preference :
         {RDGQueuePreference::eDefault, RDGQueuePreference::ePreferAsyncCompute})
    {
        RenderGraph graph("copy_preference");
        ASSERT_TRUE(graph.Begin());
        RDGBuffer input  = graph.GetResourceManager()->ImportHostWrittenBuffer(source);
        RDGBuffer target = graph.GetResourceManager()->ImportBuffer(output);
        graph.AddTransferPass("copy")
            .SetQueuePreference(preference)
            .CopyBuffer(input, target, {0, 0, 64});
        ASSERT_TRUE(graph.End());
        ASSERT_TRUE(device->ExecuteRenderGraph(graph));
        const RDGNodeMetrics& metrics = device->GetRDGMetrics().GetLastSnapshot().nodes[0];
        EXPECT_EQ(metrics.queuePreference, preference);
        EXPECT_EQ(metrics.asyncComputeEligibility,
                  preference == RDGQueuePreference::eDefault ?
                      RDGAsyncComputeEligibility::eNotRequested :
                      RDGAsyncComputeEligibility::eEligible);
        EXPECT_EQ(output->bytes, source->bytes);
    }
    EXPECT_EQ(rhi->submitted[size_t(RHICommandContextType::eAsyncCompute)], 1u);
    EXPECT_EQ(rhi->submitted[size_t(RHICommandContextType::eGraphics)], 0u);
    EXPECT_EQ(rhi->submitted[size_t(RHICommandContextType::eTransfer)], 1u);
    device->DestroyBuffer(source);
    device->DestroyBuffer(output);
}

TEST_P(RDGQueuePreferenceTest, ImportedResourceContractsAndViewportProduceExplicitFallbacks)
{
    for (int scenario = 0; scenario < 4; ++scenario)
    {
        RenderGraph graph("resource_contract");
        ASSERT_TRUE(graph.Begin());
        TestTexture* texture            = static_cast<TestTexture*>(Texture());
        texture->asyncComputeAccessible = scenario != 1;
        viewport.color                  = scenario == 3 ? texture : nullptr;
        RDGResourceManager* resources   = graph.GetResourceManager();
        RDGTexture imported             = scenario == 2 ?
            resources->ImportTexture(texture, RDGTextureImportState{}) :
            resources->ImportTexture(texture);
        graph.AddTransferPass("clear")
            .SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute)
            .ClearTexture(imported, Color(0.f));
        ASSERT_TRUE(graph.End());
        ASSERT_TRUE(device->ExecuteRenderGraph(graph));
        const RDGAsyncComputeEligibility expected[] = {
            RDGAsyncComputeEligibility::eEligible, RDGAsyncComputeEligibility::eResourceUnavailable,
            RDGAsyncComputeEligibility::eExternalState,
            RDGAsyncComputeEligibility::eViewportResource};
        EXPECT_EQ(device->GetRDGMetrics().GetLastSnapshot().nodes[0].asyncComputeEligibility,
                  expected[scenario]);
        viewport.color = nullptr;
        device->DestroyTexture(texture);
    }
}

TEST_P(RDGQueuePreferenceTest, ImportedBufferContractAppliesToComputeBindings)
{
    TestBuffer* buffer             = Buffer();
    buffer->asyncComputeAccessible = false;
    RenderGraph graph("buffer_contract");
    ASSERT_TRUE(graph.Begin());
    RDGComputePassDesc pass = IntentPass();
    pass.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
    pass.BindStorageBuffer("write_buffer", graph.GetResourceManager()->ImportBuffer(buffer),
                           RDGContentGuarantee::eFullWrite);
    graph.AddComputePass(pass);
    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(device->GetRDGMetrics().GetLastSnapshot().nodes[0].asyncComputeEligibility,
              RDGAsyncComputeEligibility::eResourceUnavailable);
    device->DestroyBuffer(buffer);
}

TEST_P(RDGQueuePreferenceTest, RebuildInvalidatesPlansAndResetsReusedPassPreference)
{
    using Access = RDGExecutionPlanTestAccess;
    RenderGraph graph("rebuild_preference");
    RDGExecutor executor(device), other(device);
    ASSERT_TRUE(graph.Begin());
    graph.AddComputePass(IntentPass().SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute));
    ASSERT_TRUE(graph.End());
    Access::Plan old;
    const uint64_t progressQueries = rhi->progressQueries;
    ASSERT_TRUE(Access::Prepare(executor, graph, old));
    RDGComputePass* storage = Access::Compute(graph);
    ASSERT_TRUE(other.Prepare(&graph));
    ASSERT_TRUE(Access::Refresh(executor, old));
    EXPECT_EQ(old.preparationPasses, 2u);
    EXPECT_EQ(Access::CompiledNode(graph).asyncComputeEligibility,
              RDGAsyncComputeEligibility::eEligible);
    EXPECT_EQ(Access::CompiledNode(graph).queuePreference, RDGQueuePreference::ePreferAsyncCompute);
    EXPECT_EQ(rhi->progressQueries, progressQueries);
    ASSERT_TRUE(graph.Begin());
    Access::CheckIdle(graph);
    graph.AddComputePass(IntentPass());
    ASSERT_TRUE(graph.End());
    Access::Plan current;
    ASSERT_TRUE(Access::Prepare(executor, graph, current));
    EXPECT_EQ(Access::Compute(graph), storage);
    EXPECT_EQ(storage->queuePreference, RDGQueuePreference::eDefault);
    EXPECT_EQ(Access::CompiledNode(graph).asyncComputeEligibility,
              RDGAsyncComputeEligibility::eNotRequested);
    RHICommandList commands;
    EXPECT_FALSE(Access::Execute(executor, old, commands));
    EXPECT_EQ(commands.GetCommandCount(), 0u);
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eLifecycle);
}

TEST_P(RDGQueuePreferenceTest, InvalidPreferencesAndStaleRecordersAreRejected)
{
    for (bool compute : {false, true})
    {
        RenderGraph graph("invalid_preference");
        ASSERT_TRUE(graph.Begin());
        const RDGQueuePreference invalid = static_cast<RDGQueuePreference>(255);
        if (compute)
        {
            graph.AddComputePass(IntentPass().SetQueuePreference(invalid));
        }
        else
        {
            graph.AddTransferPass("invalid").SetQueuePreference(invalid);
        }
        EXPECT_FALSE(graph.End());
        EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eBinding);
    }
    RenderGraph graph("stale_preference");
    ASSERT_TRUE(graph.Begin());
    RDGTransferPassCmdRecorder recorder = graph.AddTransferPass("old");
    ASSERT_TRUE(graph.Reset());
    ASSERT_TRUE(graph.Begin());
    graph.AddTransferPass("new");
    recorder.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eLifecycle);
    EXPECT_EQ(RDGExecutionPlanTestAccess::RecordedNode(graph).queuePreference,
              RDGQueuePreference::eDefault);
}

TEST_P(RDGQueuePreferenceTest, PreferenceDoesNotKeepDeadPassesOrResourcesAlive)
{
    RenderGraph graph("dead_preferred_pass");
    ASSERT_TRUE(graph.Begin());
    const RDGTexture texture = graph.GetResourceManager()->CreateTexture(LogicalTexture());
    graph.AddTransferPass("dead_clear")
        .SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute)
        .ClearTexture(texture, Color(0.f));
    RDGComputePassDesc compute = IntentPass();
    compute.allowCulling       = true;
    compute.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
    compute.BindStorageImage("write_image", texture, {}, RDGContentGuarantee::eFullWrite);
    graph.AddComputePass(compute);
    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(graph.GetCompileStats().culledPassCount, 2u);
    EXPECT_EQ(rhi->textureCreations, 0u);
}

INSTANTIATE_TEST_SUITE_P(InlineAndThreaded,
                         RDGQueuePreferenceTest,
                         testing::Values(RHIExecutionMode::eInline, RHIExecutionMode::eThreaded));

class RDGQueuePreferenceCapabilityTest :
    public RenderCoreTest,
    public testing::WithParamInterface<std::tuple<RHIExecutionMode, int>>
{
protected:
    void SetUp() override
    {
        const int scenario                 = std::get<1>(GetParam());
        RHIQueueCapabilities queues        = DistinctComputeQueues();
        queues.computeSupported            = scenario != 2;
        queues.asyncSubmissionDependencies = scenario != 4;
        if (scenario == 3)
        {
            queues.queueIds[1] = queues.queueIds[0];
        }
        RHIQueueCopyCapabilities compute{scenario == 5, scenario != 7, true, {1, 1, 1}};
        if (scenario == 6)
        {
            compute.minImageTransferGranularity.fill(4);
        }
        InitializeDevice(nullptr, 2, std::get<0>(GetParam()), true,
                         scenario == 1 ? AsyncComputeMode::eDisabled : AsyncComputeMode::eAuto,
                         queues, compute);
        CaptureVersionGraph(device);
        CreateTestShaderProgram(device, "intent");
    }
};

TEST_P(RDGQueuePreferenceCapabilityTest, PolicyAndCommandCapabilitiesResolveBeforeMaterialization)
{
    const int scenario                          = std::get<1>(GetParam());
    const RDGAsyncComputeEligibility expected[] = {
        RDGAsyncComputeEligibility::eEligible,
        RDGAsyncComputeEligibility::ePolicyDisabled,
        RDGAsyncComputeEligibility::eComputeUnavailable,
        RDGAsyncComputeEligibility::eSharedGraphicsQueue,
        RDGAsyncComputeEligibility::eDependenciesUnavailable,
        RDGAsyncComputeEligibility::eEligible,
        RDGAsyncComputeEligibility::eUnsupportedCommands,
        RDGAsyncComputeEligibility::eUnsupportedCommands};
    RenderGraph graph("capability_fallback");
    ASSERT_TRUE(graph.Begin());
    TestBuffer* source            = Buffer(256);
    RDGResourceManager* resources = graph.GetResourceManager();
    RDGTexture texture            = resources->CreateTexture(LogicalTexture(3));
    {
        RDGTransferPassCmdRecorder recorder = graph.AddTransferPass("preferred_transfer");
        recorder.NeverCull().SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
        recorder.ClearTexture(texture, Color(0.f));
        if (scenario == 5)
        {
            recorder.GenerateMipmaps(texture);
        }
        else
        {
            RHIBufferTextureCopyRegion region{};
            region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
            region.textureSubresources.layerCount = 1;
            region.textureOffset                  = {1, 0, 0};
            region.textureSize                    = {1, 1, 1};
            recorder.CopyBufferToTexture(resources->ImportHostWrittenBuffer(source), texture,
                                         region);
        }
    }
    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    const RDGNodeMetrics& metrics = device->GetRDGMetrics().GetLastSnapshot().nodes[0];
    EXPECT_EQ(metrics.queuePreference, RDGQueuePreference::ePreferAsyncCompute);
    EXPECT_EQ(metrics.asyncComputeEligibility, expected[scenario]);
    EXPECT_NE(RDGMetrics::Format(device->GetRDGMetrics().GetLastSnapshot())
                  .find(AsyncComputeEligibilityName(expected[scenario])),
              std::string::npos);
    EXPECT_EQ(rhi->submitted[size_t(RHICommandContextType::eAsyncCompute)],
              expected[scenario] == RDGAsyncComputeEligibility::eEligible ? 1u : 0u);
    device->DestroyBuffer(source);
}

INSTANTIATE_TEST_SUITE_P(InlineAndThreaded,
                         RDGQueuePreferenceCapabilityTest,
                         testing::Combine(testing::Values(RHIExecutionMode::eInline,
                                                          RHIExecutionMode::eThreaded),
                                          testing::Range(0, 8)));
} // namespace
