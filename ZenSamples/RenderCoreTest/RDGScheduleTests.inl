namespace
{
class RDGScheduleTest : public RDGQueuePreferenceTest
{};

void ConfigureScheduledBufferPass(RDGPassDescBase& pass, NameID tag, RDGBuffer input, RDGBuffer output)
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

void AddScheduledBufferPass(RenderGraph&          graph,
                            NameID                tag,
                            RHICommandContextType queue,
                            RDGBuffer             input  = {},
                            RDGBuffer             output = {})
{
    if (queue == RHICommandContextType::eGraphics)
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

bool HasScheduledDependency(const RDGSchedule& schedule, RDG_ID source, RDG_ID destination, RDGDependencyReason reason)
{
    bool found = false;
    for (const RDGDependency& dependency : schedule.dependencies)
    {
        found |= dependency.source == source && dependency.destination == destination && dependency.reason == reason;
    }
    return found;
}

void ExpectPredecessor(const RDGSchedule& schedule, uint32_t source, uint32_t destination, bool expected = true)
{
    ASSERT_LT(destination, schedule.groups.size());
    const HeapVector<uint32_t>& predecessors = schedule.groups[destination].predecessors;
    EXPECT_EQ(std::find(predecessors.begin(), predecessors.end(), source) != predecessors.end(), expected);
}

TEST_P(RDGScheduleTest, GraphicsComputeGraphicsPreservesProducerDependencies)
{
    RenderGraph graph("graphics_compute_graphics");
    ASSERT_TRUE(graph.Begin());
    RDGResourceManager* resources = graph.GetResourceManager();
    const RDGBuffer     first     = resources->CreateBuffer(LogicalBuffer());
    const RDGBuffer     second    = resources->CreateBuffer(LogicalBuffer());
    AddScheduledBufferPass(graph, "graphics_producer", RHICommandContextType::eGraphics, {}, first);
    AddScheduledBufferPass(graph, "compute", RHICommandContextType::eAsyncCompute, first, second);
    AddScheduledBufferPass(graph, "graphics_consumer", RHICommandContextType::eGraphics, second);
    ASSERT_TRUE(graph.End());
    RDGExecutor                      executor(device);
    RDGExecutionPlanTestAccess::Plan plan;
    ASSERT_TRUE(RDGExecutionPlanTestAccess::Prepare(executor, graph, plan));
    ASSERT_EQ(plan.schedule.groups.size(), 3u);
    EXPECT_EQ(plan.schedule.groups[0].queue, RHICommandContextType::eGraphics);
    EXPECT_EQ(plan.schedule.groups[1].queue, RHICommandContextType::eAsyncCompute);
    EXPECT_EQ(plan.schedule.groups[2].queue, RHICommandContextType::eGraphics);
    ExpectPredecessor(plan.schedule, 0, 1);
    ExpectPredecessor(plan.schedule, 1, 2);
    ExpectPredecessor(plan.schedule, 0, 2);
    EXPECT_TRUE(plan.schedule.usesMultipleQueues);
    EXPECT_TRUE(plan.schedule.allowsAllocationReuse);
    EXPECT_EQ(rhi->submissionAttempts, 0u);
}

TEST_P(RDGScheduleTest, QueueOrderStartsForksAndDefersJoinsWithoutChangingVersionDependencies)
{
    for (AsyncComputeMode policy : {AsyncComputeMode::eAuto, AsyncComputeMode::eDisabled})
    {
        ASSERT_TRUE(device->SetAsyncComputeMode(policy));

        RenderGraph graph("queue_aware_fork_join");

        for (uint32_t repeat = 0; repeat < 3; ++repeat)
        {
            ASSERT_TRUE(graph.Begin());

            RDGResourceManager* resources = graph.GetResourceManager();

            const RDGBuffer independent   = resources->CreateBuffer(LogicalBuffer());

            const RDGBuffer first         = resources->CreateBuffer(LogicalBuffer());

            const RDGBuffer second        = resources->CreateBuffer(LogicalBuffer());

            AddScheduledBufferPass(graph, "independent_root", RHICommandContextType::eGraphics, {}, independent);

            AddScheduledBufferPass(graph, "fork_producer", RHICommandContextType::eGraphics, {}, first);

            AddScheduledBufferPass(graph, "compute_chain", RHICommandContextType::eAsyncCompute, first, second);

            AddScheduledBufferPass(graph, "join", RHICommandContextType::eGraphics, second);

            AddScheduledBufferPass(graph, "independent_continuation", RHICommandContextType::eGraphics, independent);

            ASSERT_TRUE(graph.End());

            ASSERT_TRUE(device->ExecuteRenderGraph(graph));

            const uint32_t expectedAsync[]  = {1, 2, 0, 4, 3};

            const uint32_t expectedSerial[] = {0, 1, 4, 2, 3};

            for (uint32_t position = 0; position < 5; ++position)
            {
                EXPECT_EQ(RDGExecutionPlanTestAccess::CompiledNode(graph, position).nodeId,
                          RDG_ID(policy == AsyncComputeMode::eAuto ? expectedAsync[position] : expectedSerial[position]));
            }

            const RDGSchedule& schedule = graph.GetSchedule();

            EXPECT_TRUE(HasScheduledDependency(schedule, 0, 4, RDGDependencyReason::eVersionProducer));

            EXPECT_TRUE(HasScheduledDependency(schedule, 1, 2, RDGDependencyReason::eVersionProducer));

            EXPECT_TRUE(HasScheduledDependency(schedule, 2, 3, RDGDependencyReason::eVersionProducer));

            if (policy == AsyncComputeMode::eAuto)
            {
                const uint32_t compute          = ScheduledGroup(schedule, 2);

                const uint32_t independentGroup = ScheduledGroup(schedule, 0);

                EXPECT_NE(independentGroup, ScheduledGroup(schedule, 3));

                ExpectPredecessor(schedule, ScheduledGroup(schedule, 1), compute);

                ExpectPredecessor(schedule, compute, ScheduledGroup(schedule, 3));

                ExpectPredecessor(schedule, compute, independentGroup, false);

                ExpectPredecessor(schedule, independentGroup, compute, false);
            }
        }
    }
}

TEST_P(RDGScheduleTest, GraphicsBetweenComputeChainsDoesNotPreemptIndependentGraphics)
{
    RenderGraph graph("defer_intermediate_compute_join");

    ASSERT_TRUE(graph.Begin());

    RDGResourceManager* resources = graph.GetResourceManager();

    const RDGBuffer independent   = resources->CreateBuffer(LogicalBuffer());

    const RDGBuffer first         = resources->CreateBuffer(LogicalBuffer());

    const RDGBuffer second        = resources->CreateBuffer(LogicalBuffer());

    const RDGBuffer third         = resources->CreateBuffer(LogicalBuffer());

    AddScheduledBufferPass(graph, "independent_root", RHICommandContextType::eGraphics, {}, independent);

    AddScheduledBufferPass(graph, "first_compute", RHICommandContextType::eAsyncCompute, {}, first);

    AddScheduledBufferPass(graph, "intermediate_join", RHICommandContextType::eGraphics, first, second);

    AddScheduledBufferPass(graph, "second_compute", RHICommandContextType::eAsyncCompute, second, third);

    AddScheduledBufferPass(graph, "independent_continuation", RHICommandContextType::eGraphics, independent);

    AddScheduledBufferPass(graph, "final_join", RHICommandContextType::eGraphics, third);

    ASSERT_TRUE(graph.End());

    ASSERT_TRUE(device->ExecuteRenderGraph(graph));

    const uint32_t expected[] = {1, 0, 4, 2, 3, 5};

    for (uint32_t position = 0; position < 6; ++position)
    {
        EXPECT_EQ(RDGExecutionPlanTestAccess::CompiledNode(graph, position).nodeId, RDG_ID(expected[position]));
    }

    const RDGSchedule& schedule = graph.GetSchedule();

    ExpectPredecessor(schedule, ScheduledGroup(schedule, 1), ScheduledGroup(schedule, 2));

    ExpectPredecessor(schedule, ScheduledGroup(schedule, 2), ScheduledGroup(schedule, 3));

    ExpectPredecessor(schedule, ScheduledGroup(schedule, 3), ScheduledGroup(schedule, 5));

    ExpectPredecessor(schedule, ScheduledGroup(schedule, 1), ScheduledGroup(schedule, 0), false);
}

TEST_P(RDGScheduleTest, ResetAndComputeCoalesceWithoutMakingIndependentGraphicsWait)
{
    RenderGraph graph("voxel_shape");
    ASSERT_TRUE(graph.Begin());
    RDGResourceManager* resources = graph.GetResourceManager();
    const RDGTexture    volume    = resources->CreateTexture(LogicalTexture());
    const RDGBuffer     data      = resources->CreateBuffer(LogicalBuffer());
    AddScheduledBufferPass(graph, "independent_skybox", RHICommandContextType::eGraphics);
    graph.AddTransferPass("reset").SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute).ClearTexture(volume, Color(0.f));
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

    const uint32_t computeGroup  = ScheduledGroup(schedule, 1);

    const uint32_t graphicsGroup = ScheduledGroup(schedule, 0);

    const uint32_t consumerGroup = ScheduledGroup(schedule, 3);

    EXPECT_EQ(computeGroup, 0u);

    EXPECT_TRUE(schedule.groups[computeGroup].predecessors.empty());

    EXPECT_TRUE(schedule.groups[graphicsGroup].predecessors.empty());

    ASSERT_EQ(schedule.groups[computeGroup].passes.size(), 2u);

    EXPECT_EQ(schedule.groups[computeGroup].passes[0].nodeId, RDG_ID(1));

    EXPECT_EQ(schedule.groups[computeGroup].passes[1].nodeId, RDG_ID(2));

    ExpectPredecessor(schedule, computeGroup, consumerGroup);

    ExpectPredecessor(schedule, graphicsGroup, consumerGroup);
    const RDGMetricsSnapshot& metrics = device->GetRDGMetrics().GetLastSnapshot();
    EXPECT_EQ(metrics.plannedGroups, 3u);
    EXPECT_TRUE(metrics.plannedMultipleQueues);
    EXPECT_TRUE(metrics.allowsAllocationReuse);
    EXPECT_NE(RDGMetrics::Format(metrics).find("planned_queue=compute group=0"), std::string::npos);
    EXPECT_EQ(rhi->submitted[size_t(RHICommandContextType::eAsyncCompute)], 1u);
}

TEST_P(RDGScheduleTest, IndependentReadyWorkCannotDelayAnEarlierProducerSignal)
{
    RenderGraph graph("signal_boundary");
    ASSERT_TRUE(graph.Begin());
    const RDGBuffer buffer = graph.GetResourceManager()->CreateBuffer(LogicalBuffer());
    AddScheduledBufferPass(graph, "producer", RHICommandContextType::eGraphics, {}, buffer);
    AddScheduledBufferPass(graph, "consumer", RHICommandContextType::eAsyncCompute, buffer);
    AddScheduledBufferPass(graph, "independent_ready_graphics", RHICommandContextType::eGraphics);
    ASSERT_TRUE(graph.End());
    RDGExecutor executor(device);
    ASSERT_TRUE(executor.Prepare(&graph));
    const RDGSchedule& schedule    = graph.GetSchedule();
    const uint32_t     producer    = ScheduledGroup(schedule, 0);
    const uint32_t     consumer    = ScheduledGroup(schedule, 1);
    const uint32_t     independent = ScheduledGroup(schedule, 2);
    ASSERT_EQ(schedule.groups.size(), 3u);
    EXPECT_NE(producer, independent);
    ExpectPredecessor(schedule, producer, consumer);
    ExpectPredecessor(schedule, independent, consumer, false);
    ExpectPredecessor(schedule, consumer, independent, false);
}

TEST_P(RDGScheduleTest, CoalescingRequiresEveryForeignConsumerToDependOnTheAppendedPass)
{
    for (uint32_t scenario = 0; scenario < 3; ++scenario)
    {
        SCOPED_TRACE(scenario);

        RenderGraph graph("consumer_dominance");

        ASSERT_TRUE(graph.Begin());

        RDGBufferDesc description = LogicalBuffer();

        description.usageFlags.SetFlag(RHIBufferUsageFlagBits::eIndirectBuffer);

        RDGResourceManager* resources = graph.GetResourceManager();

        const RDGBuffer first         = resources->CreateBuffer(description);

        const RDGBuffer second        = resources->CreateBuffer(description);

        const RDGBuffer bridge        = scenario == 1 ? resources->CreateBuffer(description) : RDGBuffer{};

        AddScheduledBufferPass(graph, "first_producer", RHICommandContextType::eGraphics, {}, first);

        AddScheduledBufferPass(graph, "second_producer", RHICommandContextType::eGraphics, {}, second);

        RDGComputePassDesc consumer = IntentPass("first_consumer");

        consumer.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);

        consumer.BindStorageBuffer("read_buffer", scenario == 0 ? first : second);

        if (scenario == 1)
        {
            consumer.BindStorageBuffer("write_buffer", bridge, RDGContentGuarantee::eFullWrite);
        }
        else
        {
            consumer.UseIndirectBuffer(scenario == 0 ? second : first);
        }

        graph.AddComputePass(consumer);

        RDGComputePassDesc last = IntentPass("last_consumer");

        last.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);

        last.BindStorageBuffer("read_buffer", scenario == 0 ? second : first);

        if (scenario == 1)
        {
            last.UseIndirectBuffer(bridge);
        }

        graph.AddComputePass(last);

        ASSERT_TRUE(graph.End());

        ASSERT_TRUE(device->ExecuteRenderGraph(graph));

        const RDGSchedule& schedule = graph.GetSchedule();

        // A superset of direct consumers and a path through another compute pass
        // both permit coalescing. One uncovered consumer must preserve the boundary.
        EXPECT_EQ(schedule.groups.size(), scenario == 2 ? 4u : 2u);

        EXPECT_EQ(ScheduledGroup(schedule, 0) == ScheduledGroup(schedule, 1), scenario != 2);

        const uint32_t compute = ScheduledGroup(schedule, 2);

        EXPECT_EQ(compute == ScheduledGroup(schedule, 3), scenario != 2);

        ExpectPredecessor(schedule, ScheduledGroup(schedule, 0), compute);

        if (scenario == 2)
        {
            ExpectPredecessor(schedule, ScheduledGroup(schedule, 1), compute);

            ExpectPredecessor(schedule, ScheduledGroup(schedule, 1), ScheduledGroup(schedule, 3), false);

            EXPECT_LT(ScheduledGroup(schedule, 3), compute);
        }

        EXPECT_FALSE(device->AreSubmissionsBlocked());
    }
}

TEST_P(RDGScheduleTest, CoalescedGroupKeepsItsNewForeignConsumerBoundary)
{
    RenderGraph graph("coalesced_consumer_frontier");

    ASSERT_TRUE(graph.Begin());

    RDGBufferDesc description = LogicalBuffer();

    description.usageFlags.SetFlag(RHIBufferUsageFlagBits::eIndirectBuffer);

    RDGResourceManager* resources = graph.GetResourceManager();

    const RDGBuffer first         = resources->CreateBuffer(description);

    const RDGBuffer second        = resources->CreateBuffer(description);

    const RDGBuffer third         = resources->CreateBuffer(description);

    const RDGBuffer bridge        = resources->CreateBuffer(description);

    AddScheduledBufferPass(graph, "first", RHICommandContextType::eGraphics, {}, first);

    AddScheduledBufferPass(graph, "second", RHICommandContextType::eGraphics, {}, second);

    AddScheduledBufferPass(graph, "third", RHICommandContextType::eGraphics, {}, third);

    AddScheduledBufferPass(graph, "new_foreign_consumer", RHICommandContextType::eAsyncCompute, second, bridge);

    RDGComputePassDesc last = IntentPass("original_foreign_consumer");

    last.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);

    last.BindStorageBuffer("read_buffer", first);

    last.UseIndirectBuffer(bridge);

    last.UseIndirectBuffer(third);

    graph.AddComputePass(last);

    ASSERT_TRUE(graph.End());

    ASSERT_TRUE(device->ExecuteRenderGraph(graph));

    const RDGSchedule& schedule = graph.GetSchedule();

    ASSERT_EQ(schedule.groups.size(), 4u);

    EXPECT_EQ(ScheduledGroup(schedule, 0), ScheduledGroup(schedule, 1));

    EXPECT_NE(ScheduledGroup(schedule, 1), ScheduledGroup(schedule, 2));

    ExpectPredecessor(schedule, ScheduledGroup(schedule, 0), ScheduledGroup(schedule, 3));

    ExpectPredecessor(schedule, ScheduledGroup(schedule, 2), ScheduledGroup(schedule, 3), false);

    ExpectPredecessor(schedule, ScheduledGroup(schedule, 2), ScheduledGroup(schedule, 4));

    ExpectPredecessor(schedule, ScheduledGroup(schedule, 3), ScheduledGroup(schedule, 4));
}

TEST_P(RDGScheduleTest, ReadyGroupsExtendNativeQueueRunsWithoutMovingTheirDependencies)
{
    RenderGraph graph("stable_queue_runs");

    for (uint32_t repeat = 0; repeat < 3; ++repeat)
    {
        ASSERT_TRUE(graph.Begin());

        RDGResourceManager* resources = graph.GetResourceManager();

        const RDGBuffer first         = resources->CreateBuffer(LogicalBuffer());

        const RDGBuffer independent   = resources->CreateBuffer(LogicalBuffer());

        const RDGBuffer second        = resources->CreateBuffer(LogicalBuffer());

        AddScheduledBufferPass(graph, "graphics_producer", RHICommandContextType::eGraphics, {}, first);

        AddScheduledBufferPass(graph, "independent_compute", RHICommandContextType::eAsyncCompute, {}, independent);

        AddScheduledBufferPass(graph, "graphics_continuation", RHICommandContextType::eGraphics, first, second);

        AddScheduledBufferPass(graph, "first_compute_consumer", RHICommandContextType::eAsyncCompute, first);

        AddScheduledBufferPass(graph, "second_compute_consumer", RHICommandContextType::eAsyncCompute, second);

        AddScheduledBufferPass(graph, "graphics_join", RHICommandContextType::eGraphics, independent);

        ASSERT_TRUE(graph.End());

        ASSERT_TRUE(device->ExecuteRenderGraph(graph));

        const RDGSchedule& schedule = graph.GetSchedule();

        ASSERT_EQ(schedule.groups.size(), 6u);

        const uint32_t expectedNodes[] = {0, 2, 1, 3, 4, 5};

        for (uint32_t group = 0; group < schedule.groups.size(); ++group)
        {
            EXPECT_EQ(schedule.groups[group].id, group);

            ASSERT_EQ(schedule.groups[group].passes.size(), 1u);

            EXPECT_EQ(schedule.groups[group].passes[0].nodeId, RDG_ID(expectedNodes[group]));

            const RDGCompiledNode& compiled = RDGExecutionPlanTestAccess::CompiledNode(graph, group);

            EXPECT_EQ(compiled.submissionGroup, ScheduledGroup(schedule, compiled.nodeId));

            for (uint32_t predecessor : schedule.groups[group].predecessors)
            {
                EXPECT_LT(predecessor, group);
            }
        }

        ExpectPredecessor(schedule, 0, 1);

        ExpectPredecessor(schedule, 2, 1, false);

        ExpectPredecessor(schedule, 0, 3);

        ExpectPredecessor(schedule, 1, 4);

        ExpectPredecessor(schedule, 2, 5);

        EXPECT_FALSE(device->AreSubmissionsBlocked());
    }
}

TEST_P(RDGScheduleTest, IndependentQueuesNeverShareBufferOrTextureStorage)
{
    for (int mode = 0; mode < 3; ++mode)
    {
        for (bool texture : {false, true})
        {
            RenderGraph graph("allocation_gate");
            ASSERT_TRUE(graph.Begin());
            RDGResourceManager* resources = graph.GetResourceManager();
            RDGResource         first, second;
            if (texture)
            {
                const RDGTexture a = resources->CreateTexture(LogicalTexture());
                const RDGTexture b = resources->CreateTexture(LogicalTexture());
                first              = a;
                second             = b;
                graph.AddTransferPass("first")
                    .NeverCull()
                    .SetQueuePreference(mode != 0 ? RDGQueuePreference::ePreferAsyncCompute : RDGQueuePreference::eDefault)
                    .ClearTexture(a, Color(0.f));
                graph.AddTransferPass("second")
                    .NeverCull()
                    .SetQueuePreference(mode == 2 ? RDGQueuePreference::ePreferAsyncCompute : RDGQueuePreference::eDefault)
                    .ClearTexture(b, Color(0.f));
            }
            else
            {
                const RDGBuffer a = resources->CreateBuffer(LogicalBuffer());
                const RDGBuffer b = resources->CreateBuffer(LogicalBuffer());
                first             = a;
                second            = b;
                AddScheduledBufferPass(
                    graph, "first", mode != 0 ? RHICommandContextType::eAsyncCompute : RHICommandContextType::eGraphics, {}, a);
                AddScheduledBufferPass(graph, "second",
                                       mode == 2 ? RHICommandContextType::eAsyncCompute : RHICommandContextType::eGraphics, {},
                                       b);
            }
            ASSERT_TRUE(graph.End());
            RDGExecutor executor(device);
            ASSERT_TRUE(executor.Prepare(&graph));
            const bool multiple = mode == 1;
            EXPECT_EQ(graph.GetSchedule().usesMultipleQueues, multiple);
            EXPECT_EQ(graph.GetCompileStats().reusedAllocationCount, multiple ? 0u : 1u);
            EXPECT_EQ(DescribeResource(resources, first).physicalStableId
                          == DescribeResource(resources, second).physicalStableId,
                      !multiple);
        }
    }
}

TEST_P(RDGScheduleTest, MultiQueueGraphReusesDisjointLifetimesWithinEachQueue)
{
    for (bool texture : {false, true})
    {
        for (bool reuse : {false, true})
        {
            SCOPED_TRACE(texture);

            SCOPED_TRACE(reuse);

            RenderGraph graph("queue_local_reuse");

            ASSERT_TRUE(graph.SetOptimizations(true, reuse));

            ASSERT_TRUE(graph.Begin());

            RDGResourceManager* resources = graph.GetResourceManager();

            RDGResource allocations[2][2];

            for (uint32_t use = 0; use < 2; ++use)
            {
                for (uint32_t queue = 0; queue < 2; ++queue)
                {
                    if (texture)
                    {
                        const RDGTexture value  = resources->CreateTexture(LogicalTexture());

                        allocations[queue][use] = value;

                        graph.AddTransferPass("clear")
                            .NeverCull()
                            .SetQueuePreference(queue == 0 ? RDGQueuePreference::eDefault
                                                           : RDGQueuePreference::ePreferAsyncCompute)
                            .ClearTexture(value, Color(float(use)));
                    }
                    else
                    {
                        const RDGBuffer value   = resources->CreateBuffer(LogicalBuffer());

                        allocations[queue][use] = value;

                        AddScheduledBufferPass(
                            graph, "write",
                            queue == 0 ? RHICommandContextType::eGraphics : RHICommandContextType::eAsyncCompute, {}, value);
                    }
                }
            }

            ASSERT_TRUE(graph.End());

            RDGExecutor executor(device);

            ASSERT_TRUE(executor.Prepare(&graph));

            EXPECT_TRUE(graph.GetSchedule().usesMultipleQueues);

            EXPECT_EQ(graph.GetSchedule().allowsAllocationReuse, reuse);

            EXPECT_EQ(graph.GetCompileStats().reusedAllocationCount, reuse ? 2u : 0u);

            EXPECT_EQ(resources->GetPoolStats().assignedCount, reuse ? 2u : 4u);

            for (uint32_t queue = 0; queue < 2; ++queue)
            {
                EXPECT_EQ(DescribeResource(resources, allocations[queue][0]).physicalStableId
                              == DescribeResource(resources, allocations[queue][1]).physicalStableId,
                          reuse);
            }

            EXPECT_NE(DescribeResource(resources, allocations[0][0]).physicalStableId,
                      DescribeResource(resources, allocations[1][0]).physicalStableId);

            // Replaying preparation preserves the physical objects and validates their queue confinement.
            ASSERT_TRUE(executor.Prepare(&graph));

            EXPECT_EQ(graph.GetCompileStats().reusedAllocationCount, reuse ? 2u : 0u);
        }
    }
}

TEST_P(RDGScheduleTest, ResourceUsedByBothQueuesCannotDonateStorageAfterItsLastUse)
{
    for (bool texture : {false, true})
    {
        for (AsyncComputeMode policy : {AsyncComputeMode::eAuto, AsyncComputeMode::eDisabled})
        {
            ASSERT_TRUE(device->SetAsyncComputeMode(policy));

            RenderGraph graph("cross_queue_reuse_exclusion");

            ASSERT_TRUE(graph.Begin());

            RDGResourceManager* resources = graph.GetResourceManager();

            RDGResource first, last;

            if (texture)
            {
                RDGTextureDesc desc = LogicalTexture();

                desc.usageFlags.SetFlags(RHITextureUsageFlagBits::eTransferSrc, RHITextureUsageFlagBits::eTransferDst);

                const RDGTexture a = resources->CreateTexture(desc);

                const RDGTexture b = resources->CreateTexture(desc);

                desc.usageFlags.SetFlag(RHITextureUsageFlagBits::eStorage);

                const RDGTexture bridge = resources->CreateTexture(desc);

                first                   = a;

                last                    = b;

                RHITextureCopyRegion region{};

                region.size = {8, 8, 1};

                region.srcSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);

                region.dstSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);

                graph.AddTransferPass("produce")
                    .SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute)
                    .ClearTexture(a, Color(1.f));

                graph.AddTransferPass("consume").CopyTexture(a, bridge, MakeVecView(&region, 1));

                graph.AddTransferPass("later").NeverCull().CopyTexture(bridge, b, MakeVecView(&region, 1));
            }
            else
            {
                const RDGBuffer a       = resources->CreateBuffer(LogicalBuffer());

                const RDGBuffer b       = resources->CreateBuffer(LogicalBuffer());

                RDGBufferDesc desc      = LogicalBuffer();

                desc.size              *= 2;

                const RDGBuffer bridge  = resources->CreateBuffer(desc);

                first                   = a;

                last                    = b;

                AddScheduledBufferPass(graph, "produce", RHICommandContextType::eAsyncCompute, {}, a);

                AddScheduledBufferPass(graph, "consume", RHICommandContextType::eGraphics, a, bridge);

                AddScheduledBufferPass(graph, "later", RHICommandContextType::eGraphics, bridge, b);
            }

            ASSERT_TRUE(graph.End());

            RDGExecutor executor(device);

            ASSERT_TRUE(executor.Prepare(&graph));

            const bool multiple = policy == AsyncComputeMode::eAuto;

            EXPECT_EQ(graph.GetSchedule().usesMultipleQueues, multiple);

            EXPECT_EQ(graph.GetCompileStats().reusedAllocationCount, multiple ? 0u : 1u);

            EXPECT_EQ(DescribeResource(resources, first).physicalStableId == DescribeResource(resources, last).physicalStableId,
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
    AddScheduledBufferPass(graph, "old_reader", RHICommandContextType::eAsyncCompute, buffer);
    AddScheduledBufferPass(graph, "first_write", RHICommandContextType::eGraphics, {}, buffer);
    AddScheduledBufferPass(graph, "new_reader", RHICommandContextType::eAsyncCompute, buffer);
    AddScheduledBufferPass(graph, "overwrite", RHICommandContextType::eGraphics, {}, buffer);
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
    const RDGTexture    texture   = resources->ImportTexture(physical, RDGImportContents::eDefined);
    for (int reader = 0; reader < 2; ++reader)
    {
        RDGComputePassDesc pass = IntentPass();
        pass.BindSampledTexture("texture", nullptr, texture);
        pass.SetQueuePreference(reader == 0 ? RDGQueuePreference::eDefault : RDGQueuePreference::ePreferAsyncCompute);
        graph.AddComputePass(pass);
    }
    RHITextureCopyRegion copy{};
    copy.size = {8, 8, 1};
    copy.srcSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    copy.dstSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    graph.AddTransferPass("change_to_copy_layout")
        .CopyTexture(texture, resources->ImportTexture(output), MakeVecView(&copy, 1));
    RDGComputePassDesc last = IntentPass();
    // Keep the final reader on graphics so the ready order still exercises sampled ->
    // copy -> sampled after the independent compute reader has moved to the early fork.
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
    const RDGTexture texture = graph.GetResourceManager()->ImportTexture(physical, RDGImportContents::eDefined);
    for (RDGQueuePreference preference : {RDGQueuePreference::eDefault, RDGQueuePreference::ePreferAsyncCompute})
    {
        RDGComputePassDesc pass = IntentPass();
        pass.SetQueuePreference(preference);
        pass.BindSampledTexture("texture", nullptr, texture);
        graph.AddComputePass(pass);
    }
    ASSERT_TRUE(graph.End());
    RDGExecutor  executor(device), other(device);
    Access::Plan plan;
    ASSERT_TRUE(Access::Prepare(executor, graph, plan));
    EXPECT_TRUE(HasScheduledDependency(plan.schedule, 1, 0, RDGDependencyReason::eLayoutChange));
    other.GetResourceStateTracker().UpdateTextureState(
        physical, RHIAccessMode::eRead, RHITextureUsage::eSampled,
        BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eFragmentShader));
    ASSERT_TRUE(other.Prepare(&graph));
    EXPECT_FALSE(HasScheduledDependency(graph.GetSchedule(), 1, 0, RDGDependencyReason::eLayoutChange));
    EXPECT_TRUE(HasScheduledDependency(plan.schedule, 1, 0, RDGDependencyReason::eLayoutChange));
    ASSERT_TRUE(Access::Refresh(executor, plan));
    EXPECT_TRUE(HasScheduledDependency(plan.schedule, 1, 0, RDGDependencyReason::eLayoutChange));
    executor.GetResourceStateTracker() = other.GetResourceStateTracker();
    ASSERT_TRUE(Access::Refresh(executor, plan));
    EXPECT_FALSE(HasScheduledDependency(plan.schedule, 1, 0, RDGDependencyReason::eLayoutChange));
    EXPECT_TRUE(plan.schedule.groups[1].predecessors.empty());
    device->DestroyTexture(physical);
}

TEST_P(RDGScheduleTest, IndirectAndExtractionAccessesSurviveGrouping)
{
    RenderGraph graph("indirect_and_extract");
    ASSERT_TRUE(graph.Begin());
    RDGResourceManager* resources = graph.GetResourceManager();
    const RDGBuffer     arguments = resources->CreateBuffer(LogicalBuffer());
    AddScheduledBufferPass(graph, "argument_producer", RHICommandContextType::eAsyncCompute, {}, arguments);
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
    const uint32_t     consumer = ScheduledGroup(schedule, 1);
    ASSERT_LT(consumer, schedule.groups.size());
    ExpectPredecessor(schedule, ScheduledGroup(schedule, 0), consumer);
    const RDGCompiledNode& drawNode = RDGExecutionPlanTestAccess::CompiledNode(graph, 1);
    EXPECT_TRUE(drawNode.prologueDstStages.HasFlag(RHIPipelineStageFlagBits::eDrawIndirect));
    BitField<RHIAccessFlagBits> destinationAccess;
    for (const RHIBufferTransition& transition : drawNode.prologueBufferTransitions)
    {
        destinationAccess.SetFlag(RHIBufferUsageToAccessFlagBits(transition.newUsage, transition.newAccessMode));
    }
    EXPECT_TRUE(destinationAccess.HasFlag(RHIAccessFlagBits::eIndirectCommandRead));
    EXPECT_TRUE(destinationAccess.HasFlag(RHIAccessFlagBits::eShaderRead));
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
                               multiple ? RHICommandContextType::eAsyncCompute : RHICommandContextType::eGraphics, {},
                               resources->CreateBuffer(LogicalBuffer()));
        AddScheduledBufferPass(graph, "second", RHICommandContextType::eGraphics, {}, resources->CreateBuffer(LogicalBuffer()));
        ASSERT_TRUE(graph.End());
        ASSERT_TRUE(executor.Prepare(&graph));
        EXPECT_TRUE(graph.GetSchedule().allowsAllocationReuse);
        EXPECT_EQ(graph.GetCompileStats().reusedAllocationCount, multiple ? 0u : 1u);
    }
    ASSERT_TRUE(graph.Begin());
    TestBuffer*         source    = Buffer();
    TestBuffer*         target    = Buffer();
    RDGResourceManager* resources = graph.GetResourceManager();
    graph.AddTransferPass("upload").CopyBuffer(resources->ImportHostWrittenBuffer(source), resources->ImportBuffer(target),
                                               {0, 0, 64});
    ASSERT_TRUE(graph.End());
    Access::Plan plan;
    ASSERT_TRUE(Access::Prepare(executor, graph, plan));
    EXPECT_TRUE(plan.transfer);
    ASSERT_EQ(plan.schedule.groups.size(), 1u);
    EXPECT_EQ(plan.schedule.groups[0].queue, RHICommandContextType::eTransfer);
    executor.GetResourceStateTracker().UpdateBufferState(
        target, RHIAccessMode::eReadWrite, BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eStorageBuffer),
        BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eFragmentShader));
    ASSERT_TRUE(Access::Refresh(executor, plan));
    EXPECT_FALSE(plan.transfer);
    EXPECT_EQ(plan.schedule.groups[0].queue, RHICommandContextType::eGraphics);
    device->DestroyBuffer(source);
    device->DestroyBuffer(target);
}

TEST_P(RDGScheduleTest, FailureToAllocateSecondIndependentResourceDoesNotPublishExtraction)
{
    RenderGraph graph("multi_queue_allocation_failure");
    ASSERT_TRUE(graph.Begin());
    RDGResourceManager* resources = graph.GetResourceManager();
    const RDGBuffer     a         = resources->CreateBuffer(LogicalBuffer());
    const RDGBuffer     b         = resources->CreateBuffer(LogicalBuffer());
    AddScheduledBufferPass(graph, "a", RHICommandContextType::eAsyncCompute, {}, a);
    AddScheduledBufferPass(graph, "b", RHICommandContextType::eGraphics, {}, b);
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
    RenderGraph         graph("pooled_state_refresh");
    RDGExecutor         executor(device);
    RDGResourceManager* resources   = graph.GetResourceManager();
    RDGTextureDesc      description = LogicalTexture();
    description.usageFlags.SetFlags(RHITextureUsageFlagBits::eTransferSrc, RHITextureUsageFlagBits::eTransferDst,
                                    RHITextureUsageFlagBits::eColorAttachment);
    ASSERT_TRUE(graph.Begin());
    graph.AddTransferPass("seed_pool").NeverCull().ClearTexture(resources->CreateTexture(description), Color(0.f));
    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(executor.Prepare(&graph));
    executor.GetResourceStateTracker().UpdateTextureState(
        rhi->lastCreatedTexture, RHIAccessMode::eReadWrite, RHITextureUsage::eColorAttachment,
        BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eColorAttachmentOutput));
    ASSERT_TRUE(graph.Begin());
    RHITexture*          physical = Texture();
    const RDGTexture     source   = resources->ImportTexture(physical, RDGImportContents::eDefined);
    const RDGTexture     first    = resources->CreateTexture(description);
    const RDGTexture     second   = resources->CreateTexture(description);
    RHITextureCopyRegion copy{};
    copy.size = {8, 8, 1};
    copy.srcSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    copy.dstSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    graph.AddTransferPass("default_copy").NeverCull().CopyTexture(source, first, MakeVecView(&copy, 1));
    graph.AddTransferPass("compute_copy")
        .NeverCull()
        .SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute)
        .CopyTexture(source, second, MakeVecView(&copy, 1));
    ASSERT_TRUE(graph.End());
    RDGExecutionPlanTestAccess::Plan plan;
    ASSERT_TRUE(RDGExecutionPlanTestAccess::Prepare(executor, graph, plan));
    ASSERT_EQ(plan.schedule.groups.size(), 2u);
    EXPECT_FALSE(plan.transfer);
    EXPECT_EQ(plan.schedule.groups[ScheduledGroup(plan.schedule, 0)].queue, RHICommandContextType::eGraphics);
    EXPECT_EQ(plan.schedule.groups[ScheduledGroup(plan.schedule, 1)].queue, RHICommandContextType::eAsyncCompute);
    EXPECT_TRUE(plan.schedule.allowsAllocationReuse);
    EXPECT_EQ(graph.GetCompileStats().reusedAllocationCount, 0u);
    EXPECT_NE(DescribeResource(resources, first).physicalStableId, DescribeResource(resources, second).physicalStableId);
    device->DestroyTexture(physical);
}

TEST_P(RDGScheduleTest, QueueLocalReuseSurvivesPooledTransferToGraphicsRefresh)
{
    RenderGraph graph("queue_local_pool_refresh");

    RDGExecutor executor(device);

    RDGResourceManager* resources = graph.GetResourceManager();

    RDGTextureDesc desc           = LogicalTexture();

    desc.usageFlags.SetFlags(RHITextureUsageFlagBits::eTransferDst, RHITextureUsageFlagBits::eColorAttachment);

    ASSERT_TRUE(graph.Begin());

    graph.AddTransferPass("seed").NeverCull().ClearTexture(resources->CreateTexture(desc), Color(0.f));

    ASSERT_TRUE(graph.End());

    ASSERT_TRUE(executor.Prepare(&graph));

    executor.GetResourceStateTracker().UpdateTextureState(
        rhi->lastCreatedTexture, RHIAccessMode::eReadWrite, RHITextureUsage::eColorAttachment,
        BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eColorAttachmentOutput));

    ASSERT_TRUE(graph.Begin());

    const RDGTexture first   = resources->CreateTexture(desc);

    const RDGTexture second  = resources->CreateTexture(desc);

    const RDGTexture compute = resources->CreateTexture(desc);

    graph.AddTransferPass("first").NeverCull().ClearTexture(first, Color(0.f));

    graph.AddTransferPass("second").NeverCull().ClearTexture(second, Color(1.f));

    graph.AddTransferPass("compute")
        .NeverCull()
        .SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute)
        .ClearTexture(compute, Color(0.f));

    ASSERT_TRUE(graph.End());

    ASSERT_TRUE(executor.Prepare(&graph));

    const RDGSchedule& schedule = graph.GetSchedule();

    EXPECT_TRUE(schedule.usesMultipleQueues);

    EXPECT_EQ(schedule.groups[ScheduledGroup(schedule, 0)].queue, RHICommandContextType::eGraphics);

    EXPECT_EQ(schedule.groups[ScheduledGroup(schedule, 1)].queue, RHICommandContextType::eGraphics);

    EXPECT_EQ(schedule.groups[ScheduledGroup(schedule, 2)].queue, RHICommandContextType::eAsyncCompute);

    EXPECT_EQ(graph.GetCompileStats().reusedAllocationCount, 1u);

    EXPECT_EQ(DescribeResource(resources, first).physicalStableId, DescribeResource(resources, second).physicalStableId);

    EXPECT_NE(DescribeResource(resources, first).physicalStableId, DescribeResource(resources, compute).physicalStableId);
}

TEST_P(RDGScheduleTest, BindlessSceneTextureReadsRetainProducerEdgesAndResourceSummaries)
{
    HeapVector<RHITexture*> textures{Texture(), Texture()};
    RHISampler*             sampler = rhi->CreateSampler({});
    RenderGraph             graph("scene_texture_dependencies");
    ASSERT_TRUE(graph.Begin());
    graph.AddTransferPass("upload_materials").ClearTexture(textures[0], Color(0.f)).ClearTexture(textures[1], Color(1.f));
    RDGComputePassDesc compute = IntentPass();
    compute.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
    BindSceneTextureArray(compute, sampler, textures);
    graph.AddComputePass(compute);
    ASSERT_TRUE(graph.End());
    RDGExecutor executor(device);
    ASSERT_TRUE(executor.Prepare(&graph));
    const RDGSchedule& schedule = graph.GetSchedule();
    ASSERT_EQ(schedule.groups.size(), 2u);
    EXPECT_EQ(schedule.groups[1].queue, RHICommandContextType::eAsyncCompute);
    ExpectPredecessor(schedule, 0, 1);
    uint32_t producers = 0;
    for (const RDGDependency& dependency : schedule.dependencies)
    {
        producers += dependency.source == RDG_ID(0) && dependency.destination == RDG_ID(1)
                  && dependency.reason == RDGDependencyReason::eVersionProducer;
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
    const int   scenario = std::get<1>(GetParam());
    const bool  multiple = scenario == 0 || scenario == 5 || scenario == 6;
    RenderGraph graph("placement_fallback");
    ASSERT_TRUE(graph.Begin());
    RDGResourceManager* resources = graph.GetResourceManager();
    const RDGBuffer     a         = resources->CreateBuffer(LogicalBuffer());
    const RDGBuffer     b         = resources->CreateBuffer(LogicalBuffer());
    AddScheduledBufferPass(graph, "preferred", RHICommandContextType::eAsyncCompute, {}, a);
    AddScheduledBufferPass(graph, "graphics", RHICommandContextType::eGraphics, {}, b);
    ASSERT_TRUE(graph.End());
    RDGExecutor executor(device);
    ASSERT_TRUE(executor.Prepare(&graph));
    EXPECT_EQ(graph.GetSchedule().usesMultipleQueues, multiple);
    EXPECT_EQ(graph.GetSchedule().groups.size(), multiple ? 2u : 1u);
    EXPECT_EQ(graph.GetCompileStats().reusedAllocationCount, multiple ? 0u : 1u);
    EXPECT_EQ(DescribeResource(resources, a).physicalStableId == DescribeResource(resources, b).physicalStableId, !multiple);
}

INSTANTIATE_TEST_SUITE_P(InlineAndThreaded,
                         RDGScheduleCapabilityTest,
                         testing::Combine(testing::Values(RHIExecutionMode::eInline, RHIExecutionMode::eThreaded),
                                          testing::Range(0, 8)));

class RDGScheduleAliasedQueueTest : public RDGQueuePreferenceTest
{
protected:
    void SetUp() override
    {
        RHIQueueCapabilities queues = DistinctComputeQueues();
        queues.queueIds[2]          = queues.queueIds[1];
        InitializeDevice(nullptr, 2, GetParam(), AsyncComputeMode::eAuto, queues);
    }
};

TEST_P(RDGScheduleAliasedQueueTest, ComputeTransferAliasRetainsProducerOrder)
{
    RenderGraph graph("compute_transfer_alias");
    ASSERT_TRUE(graph.Begin());
    TestBuffer*         physical    = Buffer();
    RDGResourceManager* resources   = graph.GetResourceManager();
    const RDGBuffer     source      = resources->ImportHostWrittenBuffer(physical);
    const RDGBuffer     middle      = resources->CreateBuffer(LogicalBuffer());
    const RDGBuffer     destination = resources->CreateBuffer(LogicalBuffer());
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
    EXPECT_EQ(schedule.groups[0].queue, RHICommandContextType::eTransfer);
    EXPECT_EQ(schedule.groups[1].queue, RHICommandContextType::eAsyncCompute);
    EXPECT_EQ(schedule.groups[0].queueEquivalenceId, schedule.groups[1].queueEquivalenceId);
    ExpectPredecessor(schedule, 0, 1);
    EXPECT_TRUE(schedule.allowsAllocationReuse);
    device->DestroyBuffer(physical);
}

INSTANTIATE_TEST_SUITE_P(InlineAndThreaded,
                         RDGScheduleAliasedQueueTest,
                         testing::Values(RHIExecutionMode::eInline, RHIExecutionMode::eThreaded));
} // namespace
