namespace
{
class RDGScheduleTest : public RDGQueuePreferenceTest
{};

void ConfigureScheduledBufferPass(RDGPassDescBase& pass,
                                  NameID tag,
                                  RDGBuffer input,
                                  RDGBuffer output)
{
    pass.SetShaderProgramName("intent");
    pass.SetPassTag(tag);
    if (input)
    {
        pass.BindStorageBuffer("read_buffer", input);
    }
    if (output)
    {
        pass.BindStorageBuffer("write_buffer", output, RDGContentGuarantee::eFullWrite);
    }
}

void AddScheduledBufferPass(RenderGraph& graph,
                            NameID tag,
                            RDGQueue queue,
                            RDGBuffer input  = {},
                            RDGBuffer output = {})
{
    if (queue == RDGQueue::eGraphics)
    {
        RDGGraphicsPassDesc pass;
        ConfigureScheduledBufferPass(pass, tag, input, output);
        graph.AddGraphicsPass(pass);
    }
    else
    {
        RDGComputePassDesc pass;
        ConfigureScheduledBufferPass(pass, tag, input, output);
        pass.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
        graph.AddComputePass(pass);
    }
}

uint32_t ScheduledGroup(const RDGSchedule& schedule, RDG_ID node)
{
    uint32_t result = UINT32_MAX;
    for (const RDGSubmissionGroup& group : schedule.groups)
    {
        for (const RDGScheduledPass& pass : group.passes)
        {
            if (pass.nodeId == node)
            {
                result = group.id;
            }
        }
    }
    return result;
}

bool HasScheduledDependency(const RDGSchedule& schedule,
                            RDG_ID source,
                            RDG_ID destination,
                            RDGDependencyReason reason)
{
    bool found = false;
    for (const RDGDependency& dependency : schedule.dependencies)
    {
        found |= dependency.source == source && dependency.destination == destination &&
            dependency.reason == reason;
    }
    return found;
}

void ExpectPredecessor(const RDGSchedule& schedule,
                       uint32_t source,
                       uint32_t destination,
                       bool expected = true)
{
    ASSERT_LT(destination, schedule.groups.size());
    const HeapVector<uint32_t>& predecessors = schedule.groups[destination].predecessors;
    EXPECT_EQ(std::find(predecessors.begin(), predecessors.end(), source) != predecessors.end(),
              expected);
}

TEST_P(RDGScheduleTest, GraphicsComputeGraphicsFormsAcyclicGroupsWithResourceBoundaries)
{
    RenderGraph graph("graphics_compute_graphics");
    ASSERT_TRUE(graph.Begin());
    RDGResourceManager* resources = graph.GetResourceManager();
    const RDGBuffer first         = resources->CreateBuffer(LogicalBuffer());
    const RDGBuffer second        = resources->CreateBuffer(LogicalBuffer());
    AddScheduledBufferPass(graph, "graphics_producer", RDGQueue::eGraphics, {}, first);
    AddScheduledBufferPass(graph, "compute", RDGQueue::eAsyncCompute, first, second);
    AddScheduledBufferPass(graph, "graphics_consumer", RDGQueue::eGraphics, second);
    ASSERT_TRUE(graph.End());
    RDGExecutor executor(device);
    RDGExecutionPlanTestAccess::Plan plan;
    ASSERT_TRUE(RDGExecutionPlanTestAccess::Prepare(executor, graph, plan));
    ASSERT_EQ(plan.schedule.groups.size(), 3u);
    EXPECT_EQ(plan.schedule.groups[0].queue, RDGQueue::eGraphics);
    EXPECT_EQ(plan.schedule.groups[1].queue, RDGQueue::eAsyncCompute);
    EXPECT_EQ(plan.schedule.groups[2].queue, RDGQueue::eGraphics);
    ExpectPredecessor(plan.schedule, 0, 1);
    ExpectPredecessor(plan.schedule, 1, 2);
    ExpectPredecessor(plan.schedule, 0, 2);
    ASSERT_EQ(plan.schedule.groups[1].resources.size(), 2u);
    bool producerBoundary = false;
    for (const RDGScheduleBoundary& boundary : plan.schedule.groups[1].boundaries)
    {
        producerBoundary |=
            boundary.source.nodeId == RDG_ID(0) && boundary.destination.nodeId == RDG_ID(1);
    }
    EXPECT_TRUE(producerBoundary);
    EXPECT_TRUE(plan.schedule.usesMultipleQueues);
    EXPECT_FALSE(plan.schedule.allowsAllocationReuse);
    EXPECT_EQ(rhi->submissionAttempts, 0u);
}

TEST_P(RDGScheduleTest, ResetAndComputeCoalesceWithoutMakingIndependentGraphicsWait)
{
    RenderGraph graph("voxel_shape");
    ASSERT_TRUE(graph.Begin());
    RDGResourceManager* resources = graph.GetResourceManager();
    const RDGTexture volume       = resources->CreateTexture(LogicalTexture());
    const RDGBuffer data          = resources->CreateBuffer(LogicalBuffer());
    AddScheduledBufferPass(graph, "independent_skybox", RDGQueue::eGraphics);
    graph.AddTransferPass("reset")
        .SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute)
        .ClearTexture(volume, Color(0.f));
    RDGComputePassDesc compute = IntentPass("voxel_compute");
    compute.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
    compute.BindStorageImage("image", volume);
    compute.BindStorageBuffer("write_buffer", data, RDGContentGuarantee::eProducedElements);
    graph.AddComputePass(compute);
    RDGGraphicsPassDesc draw;
    draw.SetShaderProgramName("intent");
    draw.BindSampledTexture("texture", nullptr, volume);
    draw.BindStorageBuffer("read_buffer", data, RDGContentGuarantee::eConsumeProducedElements);
    graph.AddGraphicsPass(draw);
    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    const RDGSchedule& schedule = graph.GetSchedule();
    ASSERT_EQ(schedule.groups.size(), 3u);
    EXPECT_TRUE(schedule.groups[0].predecessors.empty());
    EXPECT_TRUE(schedule.groups[1].predecessors.empty());
    ASSERT_EQ(schedule.groups[1].passes.size(), 2u);
    EXPECT_EQ(schedule.groups[1].passes[0].nodeId, RDG_ID(1));
    EXPECT_EQ(schedule.groups[1].passes[1].nodeId, RDG_ID(2));
    ExpectPredecessor(schedule, 1, 2);
    ExpectPredecessor(schedule, 0, 2);
    const RDGMetricsSnapshot& metrics = device->GetRDGMetrics().GetLastSnapshot();
    EXPECT_EQ(metrics.plannedGroups, 3u);
    EXPECT_TRUE(metrics.plannedMultipleQueues);
    EXPECT_FALSE(metrics.allowsAllocationReuse);
    EXPECT_NE(RDGMetrics::Format(metrics).find("planned_queue=compute group=1"), std::string::npos);
    EXPECT_EQ(rhi->submitted[size_t(RHICommandContextType::eAsyncCompute)], 1u);
}

TEST_P(RDGScheduleTest, IndependentReadyWorkCannotDelayAnEarlierProducerSignal)
{
    RenderGraph graph("signal_boundary");
    ASSERT_TRUE(graph.Begin());
    const RDGBuffer buffer = graph.GetResourceManager()->CreateBuffer(LogicalBuffer());
    AddScheduledBufferPass(graph, "producer", RDGQueue::eGraphics, {}, buffer);
    AddScheduledBufferPass(graph, "consumer", RDGQueue::eAsyncCompute, buffer);
    AddScheduledBufferPass(graph, "independent_ready_graphics", RDGQueue::eGraphics);
    ASSERT_TRUE(graph.End());
    RDGExecutor executor(device);
    ASSERT_TRUE(executor.Prepare(&graph));
    const RDGSchedule& schedule = graph.GetSchedule();
    const uint32_t producer     = ScheduledGroup(schedule, 0);
    const uint32_t consumer     = ScheduledGroup(schedule, 1);
    const uint32_t independent  = ScheduledGroup(schedule, 2);
    ASSERT_EQ(schedule.groups.size(), 3u);
    EXPECT_NE(producer, independent);
    ExpectPredecessor(schedule, producer, consumer);
    ExpectPredecessor(schedule, independent, consumer, false);
    ExpectPredecessor(schedule, consumer, independent, false);
}

TEST_P(RDGScheduleTest, MultiQueueMaterializationDisablesLinearBufferAndTextureReuse)
{
    for (int mode = 0; mode < 3; ++mode)
    {
        for (bool texture : {false, true})
        {
            RenderGraph graph("allocation_gate");
            ASSERT_TRUE(graph.Begin());
            RDGResourceManager* resources = graph.GetResourceManager();
            RDGResource first, second;
            if (texture)
            {
                const RDGTexture a = resources->CreateTexture(LogicalTexture());
                const RDGTexture b = resources->CreateTexture(LogicalTexture());
                first              = a;
                second             = b;
                graph.AddTransferPass("first")
                    .NeverCull()
                    .SetQueuePreference(mode != 0 ? RDGQueuePreference::ePreferAsyncCompute :
                                                    RDGQueuePreference::eDefault)
                    .ClearTexture(a, Color(0.f));
                graph.AddTransferPass("second")
                    .NeverCull()
                    .SetQueuePreference(mode == 2 ? RDGQueuePreference::ePreferAsyncCompute :
                                                    RDGQueuePreference::eDefault)
                    .ClearTexture(b, Color(0.f));
            }
            else
            {
                const RDGBuffer a = resources->CreateBuffer(LogicalBuffer());
                const RDGBuffer b = resources->CreateBuffer(LogicalBuffer());
                first             = a;
                second            = b;
                AddScheduledBufferPass(graph, "first",
                                       mode != 0 ? RDGQueue::eAsyncCompute : RDGQueue::eGraphics,
                                       {}, a);
                AddScheduledBufferPass(graph, "second",
                                       mode == 2 ? RDGQueue::eAsyncCompute : RDGQueue::eGraphics,
                                       {}, b);
            }
            ASSERT_TRUE(graph.End());
            RDGExecutor executor(device);
            ASSERT_TRUE(executor.Prepare(&graph));
            const bool multiple = mode == 1;
            EXPECT_EQ(graph.GetSchedule().usesMultipleQueues, multiple);
            EXPECT_EQ(graph.GetCompileStats().reusedAllocationCount, multiple ? 0u : 1u);
            EXPECT_EQ(DescribeResource(resources, first).physicalStableId ==
                          DescribeResource(resources, second).physicalStableId,
                      !multiple);
        }
    }
}

TEST_P(RDGScheduleTest, AllReadersAndWritersRemainDependenciesAcrossQueues)
{
    TestBuffer* physical = Buffer();
    RenderGraph graph("raw_war_waw");
    ASSERT_TRUE(graph.Begin());
    const RDGBuffer buffer = graph.GetResourceManager()->ImportHostWrittenBuffer(physical);
    AddScheduledBufferPass(graph, "old_reader", RDGQueue::eAsyncCompute, buffer);
    AddScheduledBufferPass(graph, "first_write", RDGQueue::eGraphics, {}, buffer);
    AddScheduledBufferPass(graph, "new_reader", RDGQueue::eAsyncCompute, buffer);
    AddScheduledBufferPass(graph, "overwrite", RDGQueue::eGraphics, {}, buffer);
    ASSERT_TRUE(graph.End());
    RDGExecutor executor(device);
    ASSERT_TRUE(executor.Prepare(&graph));
    const RDGSchedule& schedule = graph.GetSchedule();
    ASSERT_EQ(schedule.groups.size(), 4u);
    EXPECT_TRUE(HasScheduledDependency(schedule, 0, 1, RDGDependencyReason::eWriteAfterRead));
    EXPECT_TRUE(HasScheduledDependency(schedule, 1, 2, RDGDependencyReason::eVersionProducer));
    EXPECT_TRUE(HasScheduledDependency(schedule, 1, 3, RDGDependencyReason::eWriteAfterWrite));
    EXPECT_TRUE(HasScheduledDependency(schedule, 2, 3, RDGDependencyReason::eWriteAfterRead));
    ExpectPredecessor(schedule, 2, 3);
    device->DestroyBuffer(physical);
}

TEST_P(RDGScheduleTest, LayoutChangeWaitsForEveryReaderAndEstablishesTheNextLayout)
{
    RHITexture* physical = Texture();
    RHITexture* output   = Texture();
    RenderGraph graph("reader_layout_epochs");
    ASSERT_TRUE(graph.Begin());
    RDGResourceManager* resources = graph.GetResourceManager();
    const RDGTexture texture      = resources->ImportTexture(physical, RDGImportContents::eDefined);
    for (int reader = 0; reader < 2; ++reader)
    {
        RDGComputePassDesc pass = IntentPass();
        pass.BindSampledTexture("texture", nullptr, texture);
        pass.SetQueuePreference(reader == 0 ? RDGQueuePreference::eDefault :
                                              RDGQueuePreference::ePreferAsyncCompute);
        graph.AddComputePass(pass);
    }
    RHITextureCopyRegion copy{};
    copy.size = {8, 8, 1};
    copy.srcSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    copy.dstSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    graph.AddTransferPass("change_to_copy_layout")
        .CopyTexture(texture, resources->ImportTexture(output), MakeVecView(&copy, 1));
    RDGComputePassDesc last = IntentPass();
    last.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
    last.BindSampledTexture("texture", nullptr, texture);
    graph.AddComputePass(last);
    ASSERT_TRUE(graph.End());
    RDGExecutor executor(device);
    executor.GetResourceStateTracker().UpdateTextureState(
        physical, RHIAccessMode::eRead, RHITextureUsage::eSampled,
        BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eFragmentShader));
    ASSERT_TRUE(executor.Prepare(&graph));
    const RDGSchedule& schedule = graph.GetSchedule();
    EXPECT_FALSE(HasScheduledDependency(schedule, 0, 1, RDGDependencyReason::eLayoutChange));
    EXPECT_TRUE(HasScheduledDependency(schedule, 0, 2, RDGDependencyReason::eLayoutChange));
    EXPECT_TRUE(HasScheduledDependency(schedule, 1, 2, RDGDependencyReason::eLayoutChange));
    EXPECT_TRUE(HasScheduledDependency(schedule, 2, 3, RDGDependencyReason::eLayoutChange));
    EXPECT_TRUE(graph.GetDependencies().empty());
    device->DestroyTexture(physical);
    device->DestroyTexture(output);
}

TEST_P(RDGScheduleTest, InitialLayoutOwnerAndPlanSnapshotRefreshWithExternalState)
{
    using Access         = RDGExecutionPlanTestAccess;
    RHITexture* physical = Texture();
    RenderGraph graph("initial_layout_refresh");
    ASSERT_TRUE(graph.Begin());
    const RDGTexture texture =
        graph.GetResourceManager()->ImportTexture(physical, RDGImportContents::eDefined);
    for (RDGQueuePreference preference :
         {RDGQueuePreference::eDefault, RDGQueuePreference::ePreferAsyncCompute})
    {
        RDGComputePassDesc pass = IntentPass();
        pass.SetQueuePreference(preference);
        pass.BindSampledTexture("texture", nullptr, texture);
        graph.AddComputePass(pass);
    }
    ASSERT_TRUE(graph.End());
    RDGExecutor executor(device), other(device);
    Access::Plan plan;
    ASSERT_TRUE(Access::Prepare(executor, graph, plan));
    EXPECT_TRUE(HasScheduledDependency(plan.schedule, 0, 1, RDGDependencyReason::eLayoutChange));
    other.GetResourceStateTracker().UpdateTextureState(
        physical, RHIAccessMode::eRead, RHITextureUsage::eSampled,
        BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eFragmentShader));
    ASSERT_TRUE(other.Prepare(&graph));
    EXPECT_FALSE(
        HasScheduledDependency(graph.GetSchedule(), 0, 1, RDGDependencyReason::eLayoutChange));
    EXPECT_TRUE(HasScheduledDependency(plan.schedule, 0, 1, RDGDependencyReason::eLayoutChange));
    ASSERT_TRUE(Access::Refresh(executor, plan));
    EXPECT_TRUE(HasScheduledDependency(plan.schedule, 0, 1, RDGDependencyReason::eLayoutChange));
    executor.GetResourceStateTracker() = other.GetResourceStateTracker();
    ASSERT_TRUE(Access::Refresh(executor, plan));
    EXPECT_FALSE(HasScheduledDependency(plan.schedule, 0, 1, RDGDependencyReason::eLayoutChange));
    EXPECT_TRUE(plan.schedule.groups[1].predecessors.empty());
    device->DestroyTexture(physical);
}

TEST_P(RDGScheduleTest, IndirectAndExtractionAccessesSurviveGrouping)
{
    RenderGraph graph("indirect_and_extract");
    ASSERT_TRUE(graph.Begin());
    RDGResourceManager* resources = graph.GetResourceManager();
    const RDGBuffer arguments     = resources->CreateBuffer(LogicalBuffer());
    AddScheduledBufferPass(graph, "argument_producer", RDGQueue::eAsyncCompute, {}, arguments);
    RDGGraphicsPassDesc draw;
    ConfigureScheduledBufferPass(draw, "draw", arguments, {});
    draw.UseIndirectBuffer(arguments);
    graph.AddGraphicsPass(draw);
    RDGExtractedBuffer owner = resources->QueueBufferExtraction(arguments);
    ASSERT_TRUE(graph.End());
    RDGExecutor executor(device);
    ASSERT_TRUE(executor.Prepare(&graph));
    EXPECT_FALSE(owner);
    const RDGSchedule& schedule = graph.GetSchedule();
    const uint32_t consumer     = ScheduledGroup(schedule, 1);
    ASSERT_LT(consumer, schedule.groups.size());
    ASSERT_EQ(schedule.groups[consumer].resources.size(), 1u);
    const RDGScheduledResource& summary = schedule.groups[consumer].resources[0];
    EXPECT_TRUE(summary.stages.HasFlag(RHIPipelineStageFlagBits::eDrawIndirect));
    EXPECT_TRUE(summary.accessFlags.HasFlag(RHIAccessFlagBits::eIndirectCommandRead));
    EXPECT_TRUE(summary.accessFlags.HasFlag(RHIAccessFlagBits::eShaderRead));
    EXPECT_NE(ScheduledGroup(schedule, 2), UINT32_MAX);
    EXPECT_EQ(graph.GetCompileStats().culledPassCount, 0u);
}

TEST_P(RDGScheduleTest, RebuildRestoresReuseAndTransferRefreshReplacesQueuePlacement)
{
    using Access = RDGExecutionPlanTestAccess;
    RenderGraph graph("reuse_reset");
    RDGExecutor executor(device);
    for (bool multiple : {true, false})
    {
        ASSERT_TRUE(graph.Begin());
        RDGResourceManager* resources = graph.GetResourceManager();
        AddScheduledBufferPass(graph, "first",
                               multiple ? RDGQueue::eAsyncCompute : RDGQueue::eGraphics, {},
                               resources->CreateBuffer(LogicalBuffer()));
        AddScheduledBufferPass(graph, "second", RDGQueue::eGraphics, {},
                               resources->CreateBuffer(LogicalBuffer()));
        ASSERT_TRUE(graph.End());
        ASSERT_TRUE(executor.Prepare(&graph));
        EXPECT_EQ(graph.GetSchedule().allowsAllocationReuse, !multiple);
        EXPECT_EQ(graph.GetCompileStats().reusedAllocationCount, multiple ? 0u : 1u);
    }
    ASSERT_TRUE(graph.Begin());
    TestBuffer* source            = Buffer();
    TestBuffer* target            = Buffer();
    RDGResourceManager* resources = graph.GetResourceManager();
    graph.AddTransferPass("upload").CopyBuffer(resources->ImportHostWrittenBuffer(source),
                                               resources->ImportBuffer(target), {0, 0, 64});
    ASSERT_TRUE(graph.End());
    Access::Plan plan;
    ASSERT_TRUE(Access::Prepare(executor, graph, plan));
    EXPECT_TRUE(plan.transfer);
    ASSERT_EQ(plan.schedule.groups.size(), 1u);
    EXPECT_EQ(plan.schedule.groups[0].queue, RDGQueue::eTransfer);
    executor.GetResourceStateTracker().UpdateBufferState(
        target, RHIAccessMode::eReadWrite,
        BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eStorageBuffer),
        BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eFragmentShader));
    ASSERT_TRUE(Access::Refresh(executor, plan));
    EXPECT_FALSE(plan.transfer);
    EXPECT_EQ(plan.schedule.groups[0].queue, RDGQueue::eGraphics);
    device->DestroyBuffer(source);
    device->DestroyBuffer(target);
}

TEST_P(RDGScheduleTest, FailureToAllocateSecondIndependentResourceDoesNotPublishExtraction)
{
    RenderGraph graph("multi_queue_allocation_failure");
    ASSERT_TRUE(graph.Begin());
    RDGResourceManager* resources = graph.GetResourceManager();
    const RDGBuffer a             = resources->CreateBuffer(LogicalBuffer());
    const RDGBuffer b             = resources->CreateBuffer(LogicalBuffer());
    AddScheduledBufferPass(graph, "a", RDGQueue::eAsyncCompute, {}, a);
    AddScheduledBufferPass(graph, "b", RDGQueue::eGraphics, {}, b);
    RDGExtractedBuffer output = resources->QueueBufferExtraction(b);
    ASSERT_TRUE(graph.End());
    rhi->failBufferCreationAt = rhi->bufferCreations + 2;
    RDGExecutor executor(device);
    EXPECT_FALSE(executor.Prepare(&graph));
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eAllocation);
    EXPECT_FALSE(output);
    EXPECT_EQ(rhi->submissionAttempts, 0u);
    EXPECT_EQ(DescribeResource(resources, a).physicalStableId, 0u);
    rhi->failBufferCreationAt = 0;
}

INSTANTIATE_TEST_SUITE_P(InlineAndThreaded,
                         RDGScheduleTest,
                         testing::Values(RHIExecutionMode::eInline, RHIExecutionMode::eThreaded));

TEST_P(RDGScheduleTest, PooledGraphicsStateRefreshCannotEnableUnsafeMultiQueueReuse)
{
    RenderGraph graph("pooled_state_refresh");
    RDGExecutor executor(device);
    RDGResourceManager* resources = graph.GetResourceManager();
    RDGTextureDesc description    = LogicalTexture();
    description.usageFlags.SetFlags(RHITextureUsageFlagBits::eTransferSrc,
                                    RHITextureUsageFlagBits::eTransferDst,
                                    RHITextureUsageFlagBits::eColorAttachment);
    ASSERT_TRUE(graph.Begin());
    graph.AddTransferPass("seed_pool")
        .NeverCull()
        .ClearTexture(resources->CreateTexture(description), Color(0.f));
    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(executor.Prepare(&graph));
    executor.GetResourceStateTracker().UpdateTextureState(
        rhi->lastCreatedTexture, RHIAccessMode::eReadWrite, RHITextureUsage::eColorAttachment,
        BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eColorAttachmentOutput));
    ASSERT_TRUE(graph.Begin());
    RHITexture* physical    = Texture();
    const RDGTexture source = resources->ImportTexture(physical, RDGImportContents::eDefined);
    const RDGTexture first  = resources->CreateTexture(description);
    const RDGTexture second = resources->CreateTexture(description);
    RHITextureCopyRegion copy{};
    copy.size = {8, 8, 1};
    copy.srcSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    copy.dstSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    graph.AddTransferPass("default_copy")
        .NeverCull()
        .CopyTexture(source, first, MakeVecView(&copy, 1));
    graph.AddTransferPass("compute_copy")
        .NeverCull()
        .SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute)
        .CopyTexture(source, second, MakeVecView(&copy, 1));
    ASSERT_TRUE(graph.End());
    RDGExecutionPlanTestAccess::Plan plan;
    ASSERT_TRUE(RDGExecutionPlanTestAccess::Prepare(executor, graph, plan));
    ASSERT_EQ(plan.schedule.groups.size(), 2u);
    EXPECT_FALSE(plan.transfer);
    EXPECT_EQ(plan.schedule.groups[0].queue, RDGQueue::eGraphics);
    EXPECT_EQ(plan.schedule.groups[1].queue, RDGQueue::eAsyncCompute);
    EXPECT_FALSE(plan.schedule.allowsAllocationReuse);
    EXPECT_EQ(graph.GetCompileStats().reusedAllocationCount, 0u);
    EXPECT_NE(DescribeResource(resources, first).physicalStableId,
              DescribeResource(resources, second).physicalStableId);
    device->DestroyTexture(physical);
}

TEST_P(RDGScheduleTest, BindlessSceneTextureReadsRetainProducerEdgesAndResourceSummaries)
{
    HeapVector<RHITexture*> textures{Texture(), Texture()};
    RHISampler* sampler = rhi->CreateSampler({});
    RenderGraph graph("scene_texture_dependencies");
    ASSERT_TRUE(graph.Begin());
    graph.AddTransferPass("upload_materials")
        .ClearTexture(textures[0], Color(0.f))
        .ClearTexture(textures[1], Color(1.f));
    RDGComputePassDesc compute = IntentPass();
    compute.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
    BindSceneTextureArray(compute, sampler, textures);
    graph.AddComputePass(compute);
    ASSERT_TRUE(graph.End());
    RDGExecutor executor(device);
    ASSERT_TRUE(executor.Prepare(&graph));
    const RDGSchedule& schedule = graph.GetSchedule();
    ASSERT_EQ(schedule.groups.size(), 2u);
    EXPECT_EQ(schedule.groups[1].queue, RDGQueue::eAsyncCompute);
    EXPECT_EQ(schedule.groups[1].resources.size(), 2u);
    ExpectPredecessor(schedule, 0, 1);
    uint32_t producers = 0;
    for (const RDGDependency& dependency : schedule.dependencies)
    {
        producers += dependency.source == RDG_ID(0) && dependency.destination == RDG_ID(1) &&
            dependency.reason == RDGDependencyReason::eVersionProducer;
    }
    EXPECT_EQ(producers, 2u);
    for (RHITexture* texture : textures)
    {
        device->DestroyTexture(texture);
    }
    sampler->ReleaseReference();
}

class RDGScheduleCapabilityTest : public RDGQueuePreferenceCapabilityTest
{};

TEST_P(RDGScheduleCapabilityTest, FallbackChoosesReusePolicyBeforeAnyAllocation)
{
    const int scenario  = std::get<1>(GetParam());
    const bool multiple = scenario == 0 || scenario == 5 || scenario == 6;
    RenderGraph graph("placement_fallback");
    ASSERT_TRUE(graph.Begin());
    RDGResourceManager* resources = graph.GetResourceManager();
    const RDGBuffer a             = resources->CreateBuffer(LogicalBuffer());
    const RDGBuffer b             = resources->CreateBuffer(LogicalBuffer());
    AddScheduledBufferPass(graph, "preferred", RDGQueue::eAsyncCompute, {}, a);
    AddScheduledBufferPass(graph, "graphics", RDGQueue::eGraphics, {}, b);
    ASSERT_TRUE(graph.End());
    RDGExecutor executor(device);
    ASSERT_TRUE(executor.Prepare(&graph));
    EXPECT_EQ(graph.GetSchedule().usesMultipleQueues, multiple);
    EXPECT_EQ(graph.GetSchedule().groups.size(), multiple ? 2u : 1u);
    EXPECT_EQ(graph.GetCompileStats().reusedAllocationCount, multiple ? 0u : 1u);
    EXPECT_EQ(DescribeResource(resources, a).physicalStableId ==
                  DescribeResource(resources, b).physicalStableId,
              !multiple);
}

INSTANTIATE_TEST_SUITE_P(InlineAndThreaded,
                         RDGScheduleCapabilityTest,
                         testing::Combine(testing::Values(RHIExecutionMode::eInline,
                                                          RHIExecutionMode::eThreaded),
                                          testing::Range(0, 8)));

class RDGScheduleAliasedQueueTest : public RDGQueuePreferenceTest
{
protected:
    void SetUp() override
    {
        RHIQueueCapabilities queues = DistinctComputeQueues();
        queues.queueIds[2]          = queues.queueIds[1];
        InitializeDevice(nullptr, 2, GetParam(), true, AsyncComputeMode::eAuto, queues);
    }
};

TEST_P(RDGScheduleAliasedQueueTest, ComputeTransferAliasRetainsQueueOrderAndResourceBoundary)
{
    RenderGraph graph("compute_transfer_alias");
    ASSERT_TRUE(graph.Begin());
    TestBuffer* physical          = Buffer();
    RDGResourceManager* resources = graph.GetResourceManager();
    const RDGBuffer source        = resources->ImportHostWrittenBuffer(physical);
    const RDGBuffer middle        = resources->CreateBuffer(LogicalBuffer());
    const RDGBuffer destination   = resources->CreateBuffer(LogicalBuffer());
    graph.AddTransferPass("transfer").CopyBuffer(source, middle, {0, 0, 64});
    graph.AddTransferPass("compute_copy")
        .NeverCull()
        .SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute)
        .CopyBuffer(middle, destination, {0, 0, 64});
    ASSERT_TRUE(graph.End());
    RDGExecutor executor(device);
    ASSERT_TRUE(executor.Prepare(&graph));
    const RDGSchedule& schedule = graph.GetSchedule();
    ASSERT_EQ(schedule.groups.size(), 2u);
    EXPECT_EQ(schedule.groups[0].queue, RDGQueue::eTransfer);
    EXPECT_EQ(schedule.groups[1].queue, RDGQueue::eAsyncCompute);
    EXPECT_EQ(schedule.groups[0].queueEquivalenceId, schedule.groups[1].queueEquivalenceId);
    ExpectPredecessor(schedule, 0, 1);
    bool retained = false;
    for (const RDGScheduleBoundary& boundary : schedule.groups[1].boundaries)
    {
        retained |= boundary.source.nodeId == RDG_ID(0) &&
            boundary.destination.nodeId == RDG_ID(1) &&
            boundary.source.accessFlags.HasFlag(RHIAccessFlagBits::eTransferWrite) &&
            boundary.destination.accessFlags.HasFlag(RHIAccessFlagBits::eTransferRead);
    }
    EXPECT_TRUE(retained);
    EXPECT_FALSE(schedule.allowsAllocationReuse);
    device->DestroyBuffer(physical);
}

INSTANTIATE_TEST_SUITE_P(InlineAndThreaded,
                         RDGScheduleAliasedQueueTest,
                         testing::Values(RHIExecutionMode::eInline, RHIExecutionMode::eThreaded));
} // namespace
