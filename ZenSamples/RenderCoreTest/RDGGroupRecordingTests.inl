namespace
{
using GroupAccess = RDGExecutionPlanTestAccess;

struct RecordedGroupLists
{
    RenderDevice& device;
    HeapVector<RHICommandList*> lists;

    RecordedGroupLists(RenderDevice& owner, const RDGSchedule& schedule) : device(owner)
    {
        GroupAccess::AcquireGroups(device, schedule, lists);
    }

    ~RecordedGroupLists()
    {
        GroupAccess::ReleaseGroups(device, lists);
    }

    void Replay()
    {
        for (RHICommandList* list : lists)
        {
            GetRHIThread().Invoke(&RHICommandList::Execute, list);
        }
    }
};

void ConfigureGroupMetrics(RDGExecutor& executor)
{
    RDGMetricsOptions options;
    options.logging.enabled      = true;
    options.logging.sampleEvery  = 1;
    options.includeTransferNodes = true;
    executor.GetMetrics().Configure(options);
    executor.GetMetrics().SetSink([](const RDGMetricsSnapshot&) {});
}

void ExpectValidGroupMetrics(const RDGExecutor& executor)
{
    const RDGMetricsSnapshot& sample = executor.GetMetrics().GetLastSnapshot();
    EXPECT_TRUE(sample.validated);
    EXPECT_EQ(sample.issues[size_t(RDGMetricIssue::eMissingBarrier)], 0u);
    EXPECT_EQ(sample.issues[size_t(RDGMetricIssue::eStageCoverage)], 0u);
    EXPECT_EQ(sample.issues[size_t(RDGMetricIssue::eAccessCoverage)], 0u);
    EXPECT_EQ(sample.issues[size_t(RDGMetricIssue::eLayoutMismatch)], 0u);
}

class RDGGroupRecordingTest : public RDGQueuePreferenceTest
{};

TEST_P(RDGGroupRecordingTest, ForeignBufferWaitRetainsIndependentLocalHazards)
{
    TestBuffer* physical = Buffer();
    RenderGraph graph("local_and_foreign_readers");
    ASSERT_TRUE(graph.Begin());
    const RDGBuffer buffer = graph.GetResourceManager()->ImportHostWrittenBuffer(physical);
    AddScheduledBufferPass(graph, "compute_read", RHICommandContextType::eAsyncCompute, buffer);
    AddScheduledBufferPass(graph, "graphics_read", RHICommandContextType::eGraphics, buffer);
    AddScheduledBufferPass(graph, "compute_write", RHICommandContextType::eAsyncCompute, {},
                           buffer);
    ASSERT_TRUE(graph.End());
    RDGExecutor executor(device);
    ConfigureGroupMetrics(executor);
    GroupAccess::Plan plan;
    ASSERT_TRUE(GroupAccess::Prepare(executor, graph, plan));
    ASSERT_EQ(plan.schedule.groups.size(), 3u);
    ExpectPredecessor(plan.schedule, 1, 2);
    ExpectPredecessor(plan.schedule, 0, 2);
    RecordedGroupLists recorded(*device, plan.schedule);
    ASSERT_TRUE(GroupAccess::ExecuteGroups(executor, plan, recorded.lists));
    ExpectValidGroupMetrics(executor);
    recorded.Replay();
    ASSERT_FALSE(rhi->compute.bufferTransitions.empty());
    EXPECT_EQ(rhi->compute.bufferTransitions.back().oldAccessMode, RHIAccessMode::eRead);
    EXPECT_EQ(rhi->compute.bufferTransitions.back().newAccessMode, RHIAccessMode::eReadWrite);
    EXPECT_TRUE(rhi->graphics.bufferTransitions.empty());
    EXPECT_EQ(rhi->submissionAttempts, 0u);
    device->DestroyBuffer(physical);
}

TEST_P(RDGGroupRecordingTest, GraphicsComputeGraphicsUsesExactGroupsAndQueuePools)
{
    RenderGraph graph("group_lists");
    ASSERT_TRUE(graph.Begin());
    const RDGBuffer buffer = graph.GetResourceManager()->CreateBuffer(LogicalBuffer());
    AddScheduledBufferPass(graph, "graphics_write", RHICommandContextType::eGraphics, {}, buffer);
    AddScheduledBufferPass(graph, "compute_read", RHICommandContextType::eAsyncCompute, buffer);
    AddScheduledBufferPass(graph, "graphics_read", RHICommandContextType::eGraphics, buffer);
    ASSERT_TRUE(graph.End());
    RDGExecutor executor(device);
    ConfigureGroupMetrics(executor);
    GroupAccess::Plan plan;
    ASSERT_TRUE(GroupAccess::Prepare(executor, graph, plan));
    ASSERT_EQ(plan.schedule.groups.size(), 3u);
    EXPECT_EQ(plan.schedule.groups[1].predecessors[0], 0u);
    RHICommandList* computeList = nullptr;
    {
        RecordedGroupLists recorded(*device, plan.schedule);
        computeList = recorded.lists[1];
        EXPECT_EQ(computeList->GetContext()->GetContextType(),
                  RHICommandContextType::eAsyncCompute);
        EXPECT_NE(recorded.lists[0], recorded.lists[2]);
        const uint64_t progressQueries = rhi->progressQueries;
        ASSERT_TRUE(GroupAccess::ExecuteGroups(executor, plan, recorded.lists));
        EXPECT_EQ(rhi->progressQueries, progressQueries);
        EXPECT_FALSE(GroupAccess::ExecuteGroups(executor, plan, recorded.lists));
        ExpectValidGroupMetrics(executor);
        recorded.Replay();
        EXPECT_TRUE(rhi->compute.bufferTransitions.empty());
        EXPECT_FALSE(rhi->graphics.bufferTransitions.empty());
    }
    RecordedGroupLists recycled(*device, plan.schedule);
    EXPECT_EQ(recycled.lists[1], computeList);
    for (RHICommandList* list : recycled.lists)
    {
        EXPECT_EQ(list->GetCommandCount(), 0u);
        EXPECT_TRUE(list->GetSubmissionDependencies().empty());
    }
}

TEST_P(RDGGroupRecordingTest, LayoutTransitionHasOneOwnerAndNoForeignGraphicsScope)
{
    RenderGraph graph("layout_fanout");
    ASSERT_TRUE(graph.Begin());
    const RDGTexture texture = graph.GetResourceManager()->CreateTexture(LogicalTexture());
    graph.AddTransferPass("graphics_clear").ClearTexture(texture, Color(0.f));
    RDGComputePassDesc compute = IntentPass("compute_storage");
    compute.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
    compute.BindStorageImage("image", texture);
    graph.AddComputePass(compute);
    RDGGraphicsPassDesc draw;
    draw.SetShaderProgramName("intent");
    draw.BindSampledTexture("texture", nullptr, texture);
    graph.AddGraphicsPass(draw);
    RDGComputePassDesc reader = IntentPass("second_reader");
    reader.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
    reader.BindSampledTexture("texture", nullptr, texture);
    graph.AddComputePass(reader);
    ASSERT_TRUE(graph.End());
    RDGExecutor executor(device);
    ConfigureGroupMetrics(executor);
    GroupAccess::Plan plan;
    ASSERT_TRUE(GroupAccess::Prepare(executor, graph, plan));
    ASSERT_EQ(plan.schedule.groups.size(), 4u);
    ExpectPredecessor(plan.schedule, 2, 3);
    RecordedGroupLists recorded(*device, plan.schedule);
    ASSERT_TRUE(GroupAccess::ExecuteGroups(executor, plan, recorded.lists));
    ExpectValidGroupMetrics(executor);
    recorded.Replay();
    uint32_t layoutChanges = 0;
    for (TestContext* context : {&rhi->graphics, &rhi->compute})
    {
        for (const RHITextureTransition& transition : context->textureTransitions)
        {
            layoutChanges += RHITextureUsageToLayout(transition.oldUsage) !=
                RHITextureUsageToLayout(transition.newUsage);
        }
    }
    EXPECT_EQ(layoutChanges, 3u); // Undefined -> clear -> storage -> sampled, once each.
    ASSERT_FALSE(rhi->compute.textureTransitions.empty());
    EXPECT_EQ(int64_t(rhi->compute.textureTransitions[0].GetSourceAccess()), 0);
    for (const TestContext::BarrierBatch& batch : rhi->compute.barrierBatches)
    {
        EXPECT_TRUE(RHIQueueSupportsStages(rhi->computeCopy, batch.source));
        EXPECT_TRUE(RHIQueueSupportsStages(rhi->computeCopy, batch.destination));
    }
}

TEST_P(RDGGroupRecordingTest, ClearStorageIndirectAndVertexScopesSurviveRecording)
{
    RenderGraph graph("voxel_hazards");
    ASSERT_TRUE(graph.Begin());
    const RDGTexture image = graph.GetResourceManager()->CreateTexture(LogicalTexture());
    const RDGBuffer buffer = graph.GetResourceManager()->CreateBuffer(LogicalBuffer());
    graph.AddTransferPass("clear")
        .SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute)
        .ClearTexture(image, Color(0.f));
    RDGComputePassDesc producer = IntentPass("large_triangles");
    producer.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
    producer.BindStorageImage("image", image);
    producer.BindStorageBuffer("write_buffer", buffer, RDGContentGuarantee::eFullWrite);
    graph.AddComputePass(producer);
    RDGComputePassDesc consumer = IntentPass("predraw");
    consumer.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
    consumer.BindStorageBuffer("read_buffer", buffer);
    consumer.BindStorageImage("image", image);
    consumer.UseIndirectBuffer(buffer);
    graph.AddComputePass(consumer).RecordPassCommands(
        [buffer](RDGPassCmdEncoder& encoder) { encoder.DispatchIndirect(buffer, 0); });
    RDGGraphicsPassDesc graphics;
    graphics.SetShaderProgramName("intent");
    graphics.BindStorageBuffer("read_buffer", buffer);
    graphics.UseIndirectBuffer(buffer);
    graph.AddGraphicsPass(graphics);
    ASSERT_TRUE(graph.End());
    RDGExecutor executor(device);
    ConfigureGroupMetrics(executor);
    GroupAccess::Plan plan;
    ASSERT_TRUE(GroupAccess::Prepare(executor, graph, plan));
    RecordedGroupLists recorded(*device, plan.schedule);
    ASSERT_TRUE(GroupAccess::ExecuteGroups(executor, plan, recorded.lists));
    ExpectValidGroupMetrics(executor);
    recorded.Replay();
    bool indirect       = false;
    bool shader         = false;
    bool clearToStorage = false;
    for (const RHIBufferTransition& transition : rhi->compute.bufferTransitions)
    {
        indirect |= transition.newUsage == RHIBufferUsage::eIndirectBuffer &&
            transition.oldAccessMode == RHIAccessMode::eReadWrite;
        shader |= transition.newUsage == RHIBufferUsage::eStorageBuffer;
    }
    for (const RHITextureTransition& transition : rhi->compute.textureTransitions)
    {
        clearToStorage |= transition.oldUsage == RHITextureUsage::eTransferDst &&
            transition.newUsage == RHITextureUsage::eStorage &&
            transition.GetSourceAccess().HasFlag(RHIAccessFlagBits::eTransferWrite);
    }
    EXPECT_TRUE(indirect);
    EXPECT_TRUE(shader);
    EXPECT_TRUE(clearToStorage);
    EXPECT_EQ(rhi->compute.indirectDispatches.size(), 1u);
    EXPECT_FALSE(plan.schedule.groups.back().predecessors.empty());
}

TEST_P(RDGGroupRecordingTest, LaterCallbackFailureRollsBackEveryListAndPrivateState)
{
    for (uint32_t failure : {1u, 2u})
    {
        RenderGraph graph("group_rollback");
        ASSERT_TRUE(graph.Begin());
        TestBuffer* physical               = Buffer();
        const RDGBuffer buffer             = graph.GetResourceManager()->ImportBuffer(physical);
        uint32_t callbacks                 = 0;
        const std::thread::id renderThread = std::this_thread::get_id();
        for (uint32_t i = 0; i < 3; ++i)
        {
            RDGComputePassDesc pass = IntentPass();
            pass.SetQueuePreference(i == 1 ? RDGQueuePreference::ePreferAsyncCompute :
                                             RDGQueuePreference::eDefault);
            pass.BindStorageBuffer("write_buffer", buffer, RDGContentGuarantee::eFullWrite);
            graph.AddComputePass(pass).RecordPassCommands(
                [i, failure, &callbacks, renderThread](RDGPassCmdEncoder& encoder) {
                    EXPECT_EQ(std::this_thread::get_id(), renderThread);
                    ++callbacks;
                    encoder.Dispatch(1, 1, 1);
                    if (i == failure)
                    {
                        encoder.Fail(RDGErrorCode::eCallback, "group failure");
                    }
                });
        }
        RDGExtractedBuffer extraction = graph.GetResourceManager()->QueueBufferExtraction(buffer);
        ASSERT_TRUE(graph.End());
        RDGExecutor executor(device);
        ConfigureGroupMetrics(executor);
        executor.GetResourceStateTracker().UpdateBufferState(
            physical, RHIAccessMode::eReadWrite,
            BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eStorageBuffer),
            BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eFragmentShader));
        const RDGExternalQueueState external[] = {
            {physical->GetStableId(), RHICommandContextType::eGraphics, 11}};
        GroupAccess::Plan plan;
        ASSERT_TRUE(GroupAccess::Prepare(executor, graph, plan));
        const uint64_t revision = executor.GetResourceStateTracker().GetRevision();
        RecordedGroupLists recorded(*device, plan.schedule);
        TestBuffer* prefix = Buffer();
        HeapVector<RHICommandListBase::CommandCheckpoint> before;
        for (RHICommandList* list : recorded.lists)
        {
            list->ClearBuffer(prefix, 0, 4);
            list->AddSubmissionDependency({RHICommandContextType::eTransfer, 7});
            before.push_back(list->GetCommandCheckpoint());
        }
        EXPECT_FALSE(GroupAccess::ExecuteGroups(executor, plan, recorded.lists, external));
        EXPECT_EQ(callbacks, failure + 1);
        EXPECT_FALSE(extraction);
        EXPECT_EQ(executor.GetResourceStateTracker().GetRevision(), revision);
        EXPECT_EQ(rhi->submissionAttempts, 0u);
        for (size_t i = 0; i < recorded.lists.size(); ++i)
        {
            const RHICommandListBase::CommandCheckpoint after =
                recorded.lists[i]->GetCommandCheckpoint();
            EXPECT_EQ(after.count, before[i].count);
            EXPECT_EQ(after.resourceCount, before[i].resourceCount);
            EXPECT_EQ(after.dependencyCount, before[i].dependencyCount);
        }
        device->DestroyBuffer(prefix);
        device->DestroyBuffer(physical);
    }
}

TEST_P(RDGGroupRecordingTest, WrongContextOrRepeatedListFailsBeforeCallbacks)
{
    for (bool duplicate : {false, true})
    {
        RenderGraph graph("invalid_group_lists");
        ASSERT_TRUE(graph.Begin());
        const RDGBuffer buffer = graph.GetResourceManager()->CreateBuffer(LogicalBuffer());
        AddScheduledBufferPass(graph, "write", RHICommandContextType::eGraphics, {}, buffer);
        AddScheduledBufferPass(graph, "read", RHICommandContextType::eAsyncCompute, buffer);
        ASSERT_TRUE(graph.End());
        RDGExecutor executor(device);
        ConfigureGroupMetrics(executor);
        GroupAccess::Plan plan;
        ASSERT_TRUE(GroupAccess::Prepare(executor, graph, plan));
        RecordedGroupLists recorded(*device, plan.schedule);
        HeapVector<RHICommandList*> invalid = recorded.lists;
        if (duplicate)
        {
            invalid[1] = invalid[0];
        }
        else
        {
            std::swap(invalid[0], invalid[1]);
        }
        EXPECT_FALSE(GroupAccess::ExecuteGroups(executor, plan, invalid));
        for (RHICommandList* list : recorded.lists)
        {
            EXPECT_EQ(list->GetCommandCount(), 0u);
        }
    }
}

TEST_P(RDGGroupRecordingTest, MissingOrInvalidInitialProvenanceIsRejectedBeforeRecording)
{
    for (uint32_t scenario : {0u, 1u, 2u})
    {
        TestBuffer* physical = Buffer();
        RenderGraph graph("initial_queue_validation");
        ASSERT_TRUE(graph.Begin());
        const RDGBuffer buffer = graph.GetResourceManager()->ImportHostWrittenBuffer(physical);
        AddScheduledBufferPass(graph, "read", RHICommandContextType::eAsyncCompute, buffer);
        ASSERT_TRUE(graph.End());
        RDGExecutor executor(device);
        ConfigureGroupMetrics(executor);
        executor.GetResourceStateTracker().UpdateBufferState(
            physical, RHIAccessMode::eReadWrite,
            BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eStorageBuffer),
            BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eFragmentShader));
        GroupAccess::Plan plan;
        ASSERT_TRUE(GroupAccess::Prepare(executor, graph, plan));
        RecordedGroupLists recorded(*device, plan.schedule);
        HeapVector<RDGExternalQueueState> external;
        if (scenario != 0)
        {
            external.push_back({physical->GetStableId(), RHICommandContextType::eAsyncCompute,
                                scenario == 1 ? UINT32_MAX : 9});
        }
        // Scenario 2 supplies impossible local fragment stages on a compute-only queue.
        EXPECT_FALSE(GroupAccess::ExecuteGroups(executor, plan, recorded.lists, external));
        EXPECT_EQ(recorded.lists[0]->GetCommandCount(), 0u);
        device->DestroyBuffer(physical);
    }
}

TEST_P(RDGGroupRecordingTest, GraphicsFallbackRebuildsOrdinaryBarriers)
{
    TestBuffer* physical             = Buffer();
    physical->asyncComputeAccessible = false;
    RenderGraph graph("graphics_fallback_barriers");
    ASSERT_TRUE(graph.Begin());
    const RDGBuffer buffer = graph.GetResourceManager()->ImportBuffer(physical);
    AddScheduledBufferPass(graph, "graphics_write", RHICommandContextType::eGraphics, {}, buffer);
    AddScheduledBufferPass(graph, "preferred_read", RHICommandContextType::eAsyncCompute, buffer);
    ASSERT_TRUE(graph.End());
    RDGExecutor executor(device);
    ConfigureGroupMetrics(executor);
    GroupAccess::Plan plan;
    ASSERT_TRUE(GroupAccess::Prepare(executor, graph, plan));
    ASSERT_EQ(plan.schedule.groups.size(), 1u);
    EXPECT_EQ(plan.schedule.groups[0].queue, RHICommandContextType::eGraphics);
    EXPECT_TRUE(plan.schedule.groups[0].predecessors.empty());
    RecordedGroupLists recorded(*device, plan.schedule);
    ASSERT_TRUE(GroupAccess::ExecuteGroups(executor, plan, recorded.lists));
    ExpectValidGroupMetrics(executor);
    recorded.Replay();
    EXPECT_FALSE(rhi->graphics.bufferTransitions.empty());
    EXPECT_TRUE(rhi->compute.bufferTransitions.empty());
    device->DestroyBuffer(physical);
}

TEST_P(RDGGroupRecordingTest, InitialQueueHistoryIsPreservedAcrossForeignOverwrite)
{
    TestBuffer* physical = Buffer();
    RenderGraph graph("external_graphics_compute_graphics");
    ASSERT_TRUE(graph.Begin());
    const RDGBuffer buffer = graph.GetResourceManager()->ImportBuffer(physical);
    AddScheduledBufferPass(graph, "compute_write", RHICommandContextType::eAsyncCompute, {},
                           buffer);
    AddScheduledBufferPass(graph, "graphics_read", RHICommandContextType::eGraphics, buffer);
    RDGExtractedBuffer extraction = graph.GetResourceManager()->QueueBufferExtraction(buffer);
    ASSERT_TRUE(graph.End());
    RDGExecutor executor(device);
    ConfigureGroupMetrics(executor);
    executor.GetResourceStateTracker().UpdateBufferState(
        physical, RHIAccessMode::eReadWrite,
        BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eStorageBuffer),
        BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eFragmentShader));
    const RDGExternalQueueState external[] = {
        {physical->GetStableId(), RHICommandContextType::eGraphics, 17}};
    GroupAccess::Plan plan;
    ASSERT_TRUE(GroupAccess::Prepare(executor, graph, plan));
    RecordedGroupLists recorded(*device, plan.schedule);
    ASSERT_TRUE(GroupAccess::ExecuteGroups(executor, plan, recorded.lists, external));
    ExpectValidGroupMetrics(executor);
    EXPECT_FALSE(extraction);
    recorded.Replay();
    EXPECT_TRUE(rhi->compute.bufferTransitions.empty());
    EXPECT_FALSE(rhi->graphics.bufferTransitions.empty());
    device->DestroyBuffer(physical);
}

TEST_P(RDGGroupRecordingTest, RefreshAfterGraphicsHistoryReassignsTransferAndRebuildsBarriers)
{
    TestBuffer* source = Buffer();
    TestBuffer* target = Buffer();
    RenderGraph graph("refresh_group_barriers");
    ASSERT_TRUE(graph.Begin());
    graph.AddTransferPass("copy").CopyBuffer(
        graph.GetResourceManager()->ImportHostWrittenBuffer(source),
        graph.GetResourceManager()->ImportBuffer(target), {0, 0, 64});
    ASSERT_TRUE(graph.End());
    RDGExecutor executor(device);
    ConfigureGroupMetrics(executor);
    GroupAccess::Plan plan;
    ASSERT_TRUE(GroupAccess::Prepare(executor, graph, plan));
    EXPECT_EQ(plan.schedule.groups[0].queue, RHICommandContextType::eTransfer);
    executor.GetResourceStateTracker().UpdateBufferState(
        target, RHIAccessMode::eRead,
        BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eStorageBuffer),
        BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eFragmentShader));
    ASSERT_TRUE(GroupAccess::Refresh(executor, plan));
    EXPECT_EQ(plan.schedule.groups[0].queue, RHICommandContextType::eGraphics);
    const RDGExternalQueueState external[] = {
        {target->GetStableId(), RHICommandContextType::eGraphics, 23}};
    RecordedGroupLists recorded(*device, plan.schedule);
    ASSERT_TRUE(GroupAccess::ExecuteGroups(executor, plan, recorded.lists, external));
    ExpectValidGroupMetrics(executor);
    recorded.Replay();
    ASSERT_FALSE(rhi->graphics.barrierBatches.empty());
    EXPECT_TRUE(
        rhi->graphics.barrierBatches[0].source.HasFlag(RHIPipelineStageFlagBits::eFragmentShader));
    EXPECT_TRUE(
        rhi->graphics.barrierBatches[0].destination.HasFlag(RHIPipelineStageFlagBits::eTransfer));
    EXPECT_TRUE(rhi->transfer.barrierBatches.empty());
    device->DestroyBuffer(source);
    device->DestroyBuffer(target);
}

INSTANTIATE_TEST_SUITE_P(InlineAndThreaded,
                         RDGGroupRecordingTest,
                         testing::Values(RHIExecutionMode::eInline, RHIExecutionMode::eThreaded));

class RDGGroupQueueTest :
    public RenderCoreTest,
    public testing::WithParamInterface<std::tuple<RHIExecutionMode, bool>>
{
protected:
    TestViewport viewport;

    void SetUp() override
    {
        RHIQueueCapabilities queues = DistinctComputeQueues();
        queues.queueIds[2]          = std::get<1>(GetParam()) ? 1 : 2;
        InitializeDevice(&viewport, 2, std::get<0>(GetParam()), AsyncComputeMode::eAuto, queues);
        CreateTestShaderProgram(device, "intent");
    }
};

TEST_P(RDGGroupQueueTest, AcceptedUploadUsesLocalBarrierForAliasAndWaitForDistinctQueue)
{
    const bool alias     = std::get<1>(GetParam());
    TestBuffer* physical = Buffer();
    RHITexture* image    = Texture();
    RenderGraph graph("accepted_upload_to_compute");
    ASSERT_TRUE(graph.Begin());
    const RDGBuffer buffer =
        graph.GetResourceManager()->ImportBuffer(physical, RDGImportContents::eDefined);
    const RDGTexture texture =
        graph.GetResourceManager()->ImportTexture(image, RDGImportContents::eDefined);
    RDGComputePassDesc pass = IntentPass();
    pass.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
    pass.BindStorageBuffer("read_buffer", buffer);
    pass.BindStorageImage("image", texture);
    graph.AddComputePass(pass);
    ASSERT_TRUE(graph.End());
    RDGExecutor executor(device);
    ConfigureGroupMetrics(executor);
    executor.GetResourceStateTracker().UpdateBufferState(
        physical, RHIAccessMode::eReadWrite,
        BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferDstBuffer),
        BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eTransfer));
    executor.GetResourceStateTracker().UpdateTextureState(
        image, RHIAccessMode::eReadWrite, RHITextureUsage::eTransferDst,
        BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eTransfer));
    const RDGExternalQueueState external[] = {
        {physical->GetStableId(), RHICommandContextType::eTransfer, 12},
        {image->GetStableId(), RHICommandContextType::eTransfer, 12}};
    GroupAccess::Plan plan;
    ASSERT_TRUE(GroupAccess::Prepare(executor, graph, plan));
    RecordedGroupLists recorded(*device, plan.schedule);
    RDGSchedule recordedSchedule;
    ASSERT_TRUE(executor.ExecuteGroups(&graph, recorded.lists, recordedSchedule, external));
    ASSERT_EQ(recordedSchedule.groups[0].externalPredecessors.size(), 1u);
    EXPECT_EQ(recordedSchedule.groups[0].externalPredecessors[0], 12u);
    const RDGMetricsSnapshot& sample = executor.GetMetrics().GetLastSnapshot();
    ASSERT_EQ(sample.submissions[0].dependencies.size(), 1u);
    EXPECT_EQ(sample.submissions[0].dependencies[0].semaphore, !alias);
    ExpectValidGroupMetrics(executor);
    recorded.Replay();
    EXPECT_EQ(rhi->compute.bufferTransitions.empty(), !alias);
    ASSERT_EQ(rhi->compute.textureTransitions.size(), 1u);
    const RHITextureTransition& transition = rhi->compute.textureTransitions[0];
    EXPECT_EQ(transition.oldUsage, RHITextureUsage::eTransferDst);
    EXPECT_EQ(transition.newUsage, RHITextureUsage::eStorage);
    EXPECT_EQ(transition.GetSourceAccess().HasFlag(RHIAccessFlagBits::eTransferWrite), alias);
    EXPECT_TRUE(RHIQueueSupportsStages(rhi->computeCopy, rhi->compute.barrierSources[0]));
    device->DestroyBuffer(physical);
    device->DestroyTexture(image);
}

TEST_P(RDGGroupQueueTest, ScheduledTransferComputeAliasesKeepSubmissionOrderAndBufferBarrier)
{
    const bool alias   = std::get<1>(GetParam());
    TestBuffer* source = Buffer();
    TestBuffer* first  = Buffer();
    TestBuffer* second = Buffer();
    RenderGraph graph("scheduled_copy_groups");
    ASSERT_TRUE(graph.Begin());
    RDGResourceManager* resources = graph.GetResourceManager();
    const RDGBuffer input         = resources->ImportHostWrittenBuffer(source);
    const RDGBuffer intermediate  = resources->ImportBuffer(first);
    const RDGBuffer output        = resources->ImportBuffer(second);
    graph.AddTransferPass("upload").CopyBuffer(input, intermediate, {0, 0, 64});
    graph.AddTransferPass("compute_copy")
        .SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute)
        .CopyBuffer(intermediate, output, {0, 0, 64});
    ASSERT_TRUE(graph.End());
    RDGExecutor executor(device);
    ConfigureGroupMetrics(executor);
    GroupAccess::Plan plan;
    ASSERT_TRUE(GroupAccess::Prepare(executor, graph, plan));
    ASSERT_EQ(plan.schedule.groups.size(), 2u);
    EXPECT_EQ(plan.schedule.groups[0].queue, RHICommandContextType::eTransfer);
    EXPECT_EQ(plan.schedule.groups[1].queue, RHICommandContextType::eAsyncCompute);
    ExpectPredecessor(plan.schedule, 0, 1);
    EXPECT_EQ(plan.schedule.groups[1].predecessors.size(), 1u);
    RecordedGroupLists recorded(*device, plan.schedule);
    ASSERT_TRUE(GroupAccess::ExecuteGroups(executor, plan, recorded.lists));
    ExpectValidGroupMetrics(executor);
    recorded.Replay();
    EXPECT_EQ(rhi->compute.bufferTransitions.empty(), !alias);
    if (alias)
    {
        EXPECT_EQ(rhi->compute.bufferTransitions[0].oldUsage, RHIBufferUsage::eTransferDst);
        EXPECT_EQ(rhi->compute.bufferTransitions[0].newUsage, RHIBufferUsage::eTransferSrc);
    }
    device->DestroyBuffer(source);
    device->DestroyBuffer(first);
    device->DestroyBuffer(second);
}

INSTANTIATE_TEST_SUITE_P(InlineThreadedAndQueueSharing,
                         RDGGroupQueueTest,
                         testing::Combine(testing::Values(RHIExecutionMode::eInline,
                                                          RHIExecutionMode::eThreaded),
                                          testing::Bool()));
} // namespace
