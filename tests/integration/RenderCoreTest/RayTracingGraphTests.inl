namespace
{
ShaderProgram* CreateTracingTestProgram(RenderDevice* device, NameID name)
{
    RHIShaderCreateInfo info;
    info.spirv = MakeRefCountPtr<RHIShaderGroupSPIRV>();
    for (RHIShaderStage stage : {RHIShaderStage::eRayGeneration, RHIShaderStage::eMiss, RHIShaderStage::eMiss,
                                 RHIShaderStage::eClosestHit, RHIShaderStage::eAnyHit, RHIShaderStage::eCallable})
    {
        info.spirv->AppendOwnedStageSPIRV(stage, {1});
    }
    RegisterReflectedShader(name, info);
    return CreateTestShaderProgram(device, name);
}

HeapVector<RHIRayTracingShaderGroup> TracingTestGroups()
{
    return {{RHIRayTracingShaderGroupType::eRayGeneration, 0},
            {RHIRayTracingShaderGroupType::eMiss, 1},
            {RHIRayTracingShaderGroupType::eMiss, 2},
            {RHIRayTracingShaderGroupType::eHit, kUnusedRayTracingShader, 3, 4},
            {RHIRayTracingShaderGroupType::eCallable, 5}};
}

RHIRayTracingCapabilities TracingTestCapabilities()
{
    RHIRayTracingCapabilities result;
    result.pipeline = result.indirect = true;
    result.shaderGroupHandleSize = result.shaderGroupHandleAlignment = 32;
    result.shaderGroupBaseAlignment                                  = 64;
    result.maxShaderGroupStride                                      = 4096;
    result.maxRayRecursionDepth                                      = 16;
    result.maxRayDispatchInvocationCount                             = 1024;
    return result;
}

class RDGRayTracingTest : public RDGQueuePreferenceTest
{
protected:
    void SetUp() override
    {
        InitializeDevice(&viewport, 2, GetParam(), AsyncComputeMode::eAuto, DistinctComputeQueues(),
                         {false, true, true, {1, 1, 1}}, {}, TracingTestCapabilities());
        CreateTestShaderProgram(device, "intent");
        CaptureVersionGraph(device);
    }
};
} // namespace

TEST_F(RenderCoreTest, RayTracingCacheKeysIncludeOrderedGroupsRecursionAndShaderIdentity)
{
    ShaderProgram*                  program = CreateTracingTestProgram(device, "rt_cache");
    RHIRayTracingPipelineCreateInfo info;
    info.pShader                                      = program->GetShader();
    const HeapVector<RHIRayTracingShaderGroup> groups = TracingTestGroups();
    info.groups.assign(groups.begin(), groups.end());
    RHIPipeline* original = device->GetOrCreateRayTracingPipeline(info);
    ASSERT_NE(original, nullptr);
    EXPECT_EQ(original->GetType(), RHIPipelineType::eRayTracing);
    EXPECT_EQ(original, device->GetOrCreateRayTracingPipeline(info));
    std::swap(info.groups[1], info.groups[2]);
    EXPECT_NE(original, device->GetOrCreateRayTracingPipeline(info));
    info.groups.assign(groups.begin(), groups.end());
    info.groups[3].anyHitShader = kUnusedRayTracingShader;
    EXPECT_NE(original, device->GetOrCreateRayTracingPipeline(info));
    info.groups.assign(groups.begin(), groups.end());
    info.maxRecursionDepth = 2;
    RHIPipeline* deeper    = device->GetOrCreateRayTracingPipeline(info);
    EXPECT_NE(original, deeper);
    ASSERT_TRUE(program->Init());
    info.pShader = program->GetShader();
    EXPECT_NE(deeper, device->GetOrCreateRayTracingPipeline(info));
    info.maxRecursionDepth      = 3;
    rhi->failPipelineCreationAt = rhi->pipelineCount + 1;
    EXPECT_EQ(device->GetOrCreateRayTracingPipeline(info), nullptr);
    RHIPipeline* retry = device->GetOrCreateRayTracingPipeline(info);
    ASSERT_NE(retry, nullptr);
    EXPECT_EQ(retry, device->GetOrCreateRayTracingPipeline(info));
    const uint64_t identity = original->GetStableId();
    device->InvalidateRDGPassCompilerForResize();
    EXPECT_FALSE(destroyed.contains(identity));
    device->NextFrame();
    device->NextFrame();
    EXPECT_TRUE(destroyed.contains(identity));
}

TEST_P(RDGRayTracingTest, RayTracingUsesReflectedStagesAsyncQueuesAndIndependentLaunches)
{
    ShaderProgram* program = CreateTracingTestProgram(device, "rt_graph");
    TestShader*    shader  = static_cast<TestShader*>(program->GetShader());
    shader->SetDescriptorStages("read_buffer", RHIShaderStageFlagBits::eRayGeneration);
    shader->SetDescriptorStages("write_buffer", RHIShaderStageFlagBits::eClosestHit);
    TestBuffer*         input  = Buffer();
    TestBuffer*         output = Buffer();
    RHIBufferCreateInfo argumentInfo;
    argumentInfo.size = 16;
    argumentInfo.usageFlags.SetFlags(RHIBufferUsageFlagBits::eIndirectBuffer, RHIBufferUsageFlagBits::eDeviceAddress);
    RHIBuffer* arguments = device->CreateBuffer(argumentInfo);
    ASSERT_NE(arguments, nullptr);
    RenderGraph graph("rt_graph");
    ASSERT_TRUE(graph.Begin());
    RDGComputePassDesc producer = IntentPass("producer");
    producer.BindStorageBuffer("write_buffer", input, RDGContentGuarantee::eFullWrite);
    graph.AddComputePass(producer);
    graph.GetResourceManager()->ImportBuffer(arguments, RDGImportContents::eDefined);
    RDGRayTracingPassDesc pass;
    pass.SetPassTag("trace");
    pass.SetShaderProgramName("rt_graph");
    pass.SetShaderGroups(TracingTestGroups());
    pass.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
    pass.BindStorageBuffer("read_buffer", input);
    pass.BindStorageBuffer("write_buffer", output, RDGContentGuarantee::eFullWrite);
    pass.UseIndirectBuffer(arguments);
    pass.independentDispatches = true;
    graph.AddRayTracingPass(pass).RecordPassCommands([arguments](RDGPassCmdEncoder& encoder) {
        encoder.TraceRays(8, 4);
        encoder.TraceRaysIndirect(arguments, 4);
    });
    ASSERT_TRUE(graph.End()) << graph.GetResult().message;
    ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
    device->FlushRHIThread();
    ASSERT_EQ(rhi->compute.rayTraces.size(), 1u);
    EXPECT_EQ(rhi->compute.rayTraces[0].second, (TestDispatchExtent{8, 4, 1}));
    ASSERT_EQ(rhi->compute.indirectRayTraces.size(), 1u);
    EXPECT_EQ(rhi->compute.indirectRayTraces[0], std::make_pair(arguments, uint64_t(4)));
    const RDGMetricsSnapshot& metrics = device->GetRDGMetrics().GetLastSnapshot();
    bool                      found   = false;
    for (const RDGNodeMetrics& node : metrics.nodes)
    {
        if (node.type == RDGNodeType::eRayTracingPass)
        {
            found = true;
            EXPECT_EQ(node.asyncComputeEligibility, RDGAsyncComputeEligibility::eEligible);
        }
    }
    EXPECT_TRUE(found);
    for (RDGMetricIssue issue : {RDGMetricIssue::eMissingBarrier, RDGMetricIssue::eStageCoverage,
                                 RDGMetricIssue::eAccessCoverage, RDGMetricIssue::eOrdering, RDGMetricIssue::eInvalidSchedule})
    {
        EXPECT_EQ(metrics.issues[size_t(issue)], 0u) << RDGMetrics::Format(metrics);
    }
    EXPECT_NE(RDGMetrics::Format(metrics).find("ray_tracing"), std::string::npos);
    device->DestroyBuffer(input);
    device->DestroyBuffer(output);
    device->DestroyBuffer(arguments);
}

INSTANTIATE_TEST_SUITE_P(InlineAndThreaded,
                         RDGRayTracingTest,
                         testing::Values(RHIExecutionMode::eInline, RHIExecutionMode::eThreaded));
