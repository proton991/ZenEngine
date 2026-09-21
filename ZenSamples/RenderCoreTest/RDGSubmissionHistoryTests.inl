namespace
{
template <typename State> constexpr bool HasPublicSubmissionMutation =
    requires(State& state) { state.Queue(); } ||
    requires(State& state) { state.Accept(0, RHISubmissionDependency{}); } ||
    requires(State& state) { state.FinishSubmission(); } ||
    requires(State& state) { state.Fail(); };

static_assert(!HasPublicSubmissionMutation<RHISubmissionState>);

struct HistoryInput
{
    RDGSchedule schedule;
    HeapVector<HeapVector<RenderSubmissionAccess>> accesses;

    void Add(RHICommandContextType queue,
             uint64_t resourceId,
             RHIAccessMode mode,
             RHITextureUsage usage = RHITextureUsage::eMax)
    {
        RDGSubmissionGroup& group = schedule.groups.emplace_back();
        group.id                  = uint32_t(schedule.groups.size() - 1);
        group.queue               = queue;
        RenderSubmissionAccess access;
        access.resourceId          = resourceId;
        access.shared              = true;
        access.texture             = usage != RHITextureUsage::eMax;
        access.access.accessMode   = mode;
        access.access.textureUsage = usage;
        access.access.bufferUsage.SetFlag(
            queue != RHICommandContextType::eTransfer ? RHIBufferUsageFlagBits::eStorageBuffer :
                mode == RHIAccessMode::eRead          ? RHIBufferUsageFlagBits::eTransferSrcBuffer :
                                                        RHIBufferUsageFlagBits::eTransferDstBuffer);
        access.access.pipelineStages.SetFlag(queue == RHICommandContextType::eGraphics ?
                                                 RHIPipelineStageFlagBits::eFragmentShader :
                                                 queue == RHICommandContextType::eAsyncCompute ?
                                                 RHIPipelineStageFlagBits::eComputeShader :
                                                 RHIPipelineStageFlagBits::eTransfer);
        access.access.accessFlags.SetFlag(
            queue == RHICommandContextType::eTransfer ?
                (mode == RHIAccessMode::eRead ? RHIAccessFlagBits::eTransferRead :
                                                RHIAccessFlagBits::eTransferWrite) :
                (mode == RHIAccessMode::eRead ? RHIAccessFlagBits::eShaderRead :
                                                RHIAccessFlagBits::eShaderWrite));
        accesses.emplace_back().push_back(access);
    }
};

bool PrepareHistory(RenderSubmissionHistory& history,
                    const HistoryInput& input,
                    RenderSubmissionUpdate& update,
                    const RHIQueueCapabilities& queues = DistinctComputeQueues())
{
    return history.Prepare(input.schedule, input.accesses, queues, update);
}

void AcceptHistory(RenderSubmissionHistory& history,
                   RenderSubmissionUpdate& update,
                   VectorView<const uint64_t> serials)
{
    ASSERT_EQ(serials.size(), update.schedule.groups.size());
    ASSERT_TRUE(RHISubmissionStateTestAccess::Queue(*update.state));
    ASSERT_TRUE(history.Commit(update));
    for (size_t i = 0; i < serials.size(); ++i)
    {
        ASSERT_TRUE(RHISubmissionStateTestAccess::Accept(
            *update.state, uint32_t(i),
            {static_cast<RHICommandContextType>(update.schedule.groups[i].queue), serials[i]}));
    }
    ASSERT_TRUE(RHISubmissionStateTestAccess::FinishSubmission(*update.state));
    ASSERT_TRUE(history.ResolveAccepted());
}

void ExpectPoint(const RHISubmissionPoint& point, RHICommandContextType queue, uint64_t serial)
{
    RHISubmissionDependency resolved;
    ASSERT_EQ(point.Resolve(resolved), RHISubmissionPointStatus::eAccepted);
    EXPECT_EQ(resolved.queue, queue);
    EXPECT_EQ(resolved.serial, serial);
}

TEST(RHISubmissionPointTest, ExactGroupsRetainIndependentLogicalTimelines)
{
    const SmallVector<RHICommandContextType, 2> queues{RHICommandContextType::eAsyncCompute,
                                                       RHICommandContextType::eTransfer};
    RefCountPtr<RHISubmissionState> state = MakeRefCountPtr<RHISubmissionState>(queues);
    RHISubmissionPoint compute{queues[0], 0, state, 0};
    RHISubmissionPoint transfer{queues[1], 0, state, 1};
    RHISubmissionDependency resolved;
    EXPECT_EQ(compute.Resolve(resolved), RHISubmissionPointStatus::ePending);
    EXPECT_FALSE(RHISubmissionStateTestAccess::Accept(*state, 0, {queues[0], 91}));
    ASSERT_TRUE(RHISubmissionStateTestAccess::Queue(*state));
    EXPECT_FALSE(RHISubmissionStateTestAccess::Queue(*state));
    EXPECT_FALSE(RHISubmissionStateTestAccess::Accept(*state, 0, {queues[1], 91}));
    EXPECT_FALSE(RHISubmissionStateTestAccess::Accept(
        *state, 0, {queues[0], RHISubmissionDependency::kLatestSubmitted}));
    ASSERT_TRUE(RHISubmissionStateTestAccess::Accept(*state, 0, {queues[0], 91}));
    EXPECT_FALSE(RHISubmissionStateTestAccess::FinishSubmission(*state));
    EXPECT_EQ(transfer.Resolve(resolved), RHISubmissionPointStatus::ePending);
    ASSERT_TRUE(RHISubmissionStateTestAccess::Accept(*state, 1, {queues[1], 4}));
    ASSERT_TRUE(RHISubmissionStateTestAccess::FinishSubmission(*state));
    state.Reset();
    ExpectPoint(compute, queues[0], 91);
    ExpectPoint(transfer, queues[1], 4);
    EXPECT_FALSE(
        (RHISubmissionPoint{queues[0], RHISubmissionDependency::kLatestSubmitted}).IsValid());
}

TEST(RenderSubmissionHistoryTest, ParallelReadersProtectOverwriteIncludingAliasedTimelines)
{
    for (bool alias : {false, true})
    {
        RHIQueueCapabilities queues = DistinctComputeQueues();
        queues.queueIds[2]          = alias ? 1 : 2;
        RenderSubmissionHistory history;
        HistoryInput reads;
        reads.Add(RHICommandContextType::eAsyncCompute, 11, RHIAccessMode::eRead);
        reads.Add(RHICommandContextType::eTransfer, 11, RHIAccessMode::eRead);
        RenderSubmissionUpdate readers;
        ASSERT_TRUE(PrepareHistory(history, reads, readers, queues));
        EXPECT_TRUE(readers.schedule.groups[1].predecessors.empty());
        EXPECT_EQ(history.Find(11), nullptr);
        const SmallVector<uint64_t, 2> serials{91, 4};
        AcceptHistory(history, readers, serials);
        ASSERT_NE(history.Find(11), nullptr);
        ExpectPoint(history.Find(11)->readers[1].point, RHICommandContextType::eAsyncCompute, 91);
        ExpectPoint(history.Find(11)->readers[2].point, RHICommandContextType::eTransfer, 4);
        HistoryInput write;
        write.Add(RHICommandContextType::eGraphics, 11, RHIAccessMode::eReadWrite);
        RenderSubmissionUpdate overwrite;
        ASSERT_TRUE(PrepareHistory(history, write, overwrite, queues));
        ASSERT_EQ(overwrite.schedule.groups[0].externalPredecessors.size(), 2u);
        EXPECT_EQ(history.Find(11)->writer.point.IsValid(), false);
        const SmallVector<uint64_t, 1> graphics{7};
        AcceptHistory(history, overwrite, graphics);
        EXPECT_FALSE(history.Find(11)->readers[1].point.IsValid());
        EXPECT_FALSE(history.Find(11)->readers[2].point.IsValid());
        ExpectPoint(history.Find(11)->writer.point, RHICommandContextType::eGraphics, 7);
    }
}

TEST(RenderSubmissionHistoryTest, CurrentAndEarlierFrameReferencesStayExactBeforeAcceptance)
{
    RenderSubmissionHistory history;
    HistoryInput input;
    input.Add(RHICommandContextType::eTransfer, 22, RHIAccessMode::eReadWrite);
    input.Add(RHICommandContextType::eGraphics, 33, RHIAccessMode::eRead);
    input.Add(RHICommandContextType::eAsyncCompute, 22, RHIAccessMode::eRead);
    RenderSubmissionUpdate frame;
    ASSERT_TRUE(PrepareHistory(history, input, frame));
    EXPECT_TRUE(frame.schedule.groups[1].predecessors.empty());
    ASSERT_EQ(frame.schedule.groups[2].predecessors.size(), 1u);
    EXPECT_EQ(frame.schedule.groups[2].predecessors[0], 0u);
    EXPECT_FALSE(history.Commit(frame));
    ASSERT_TRUE(RHISubmissionStateTestAccess::Queue(*frame.state));
    ASSERT_TRUE(history.Commit(frame));
    HistoryInput overwrite;
    overwrite.Add(RHICommandContextType::eTransfer, 22, RHIAccessMode::eReadWrite);
    RenderSubmissionUpdate later;
    ASSERT_TRUE(PrepareHistory(history, overwrite, later));
    ASSERT_EQ(later.externalPoints.size(), 2u);
    EXPECT_EQ(later.externalPoints[0].state.Get(), frame.state.Get());
    EXPECT_EQ(later.externalPoints[0].group, 0u);
    EXPECT_EQ(later.externalPoints[1].state.Get(), frame.state.Get());
    EXPECT_EQ(later.externalPoints[1].group, 2u);
    RHISubmissionDependency resolved;
    EXPECT_EQ(later.externalPoints[1].Resolve(resolved), RHISubmissionPointStatus::ePending);
    ASSERT_TRUE(RHISubmissionStateTestAccess::Accept(*frame.state, 0,
                                                     {RHICommandContextType::eTransfer, 8}));
    ASSERT_TRUE(RHISubmissionStateTestAccess::Accept(*frame.state, 1,
                                                     {RHICommandContextType::eGraphics, 100}));
    ASSERT_TRUE(RHISubmissionStateTestAccess::Accept(*frame.state, 2,
                                                     {RHICommandContextType::eAsyncCompute, 3}));
    ExpectPoint(later.externalPoints[1], RHICommandContextType::eAsyncCompute, 3);
    ASSERT_TRUE(RHISubmissionStateTestAccess::FinishSubmission(*frame.state));
    ASSERT_TRUE(history.ResolveAccepted());
    EXPECT_FALSE(history.Find(22)->readers[1].point.state);
    ExpectPoint(history.Find(22)->readers[1].point, RHICommandContextType::eAsyncCompute, 3);
}

TEST(RenderSubmissionHistoryTest, LayoutChangesWaitForAllReadersAndEstablishNewReaderEpoch)
{
    RenderSubmissionHistory history;
    HistoryInput input;
    input.Add(RHICommandContextType::eGraphics, 44, RHIAccessMode::eRead,
              RHITextureUsage::eSampled);
    input.Add(RHICommandContextType::eAsyncCompute, 44, RHIAccessMode::eRead,
              RHITextureUsage::eSampled);
    RenderSubmissionUpdate readers;
    ASSERT_TRUE(PrepareHistory(history, input, readers));
    // The first reader establishes the initially undefined layout; the next consumes it.
    ASSERT_EQ(readers.schedule.groups[1].predecessors.size(), 1u);
    const SmallVector<uint64_t, 2> serials{5, 9};
    AcceptHistory(history, readers, serials);
    HistoryInput transition;
    transition.Add(RHICommandContextType::eTransfer, 44, RHIAccessMode::eRead,
                   RHITextureUsage::eTransferSrc);
    RenderSubmissionUpdate changed;
    ASSERT_TRUE(PrepareHistory(history, transition, changed));
    EXPECT_EQ(changed.schedule.groups[0].externalPredecessors.size(), 2u);
    const SmallVector<uint64_t, 1> copied{2};
    AcceptHistory(history, changed, copied);
    EXPECT_EQ(history.Find(44)->layoutUsage, RHITextureUsage::eTransferSrc);
    EXPECT_FALSE(history.Find(44)->readers[0].point.IsValid());
    EXPECT_FALSE(history.Find(44)->readers[1].point.IsValid());
    HistoryInput next;
    next.Add(RHICommandContextType::eGraphics, 44, RHIAccessMode::eRead,
             RHITextureUsage::eTransferSrc);
    RenderSubmissionUpdate read;
    ASSERT_TRUE(PrepareHistory(history, next, read));
    ASSERT_EQ(read.schedule.groups[0].externalPredecessors.size(), 1u);
    ExpectPoint(read.externalPoints[read.schedule.groups[0].externalPredecessors[0]],
                RHICommandContextType::eTransfer, 2);
}

TEST(RenderSubmissionHistoryTest, RejectionInvalidationAndPartialFailurePreserveTransactions)
{
    RenderSubmissionHistory history;
    HistoryInput input;
    input.Add(RHICommandContextType::eTransfer, 55, RHIAccessMode::eReadWrite);
    RenderSubmissionUpdate upload;
    ASSERT_TRUE(PrepareHistory(history, input, upload));
    const SmallVector<uint64_t, 1> serials{6};
    AcceptHistory(history, upload, serials);
    HistoryInput use;
    use.Add(RHICommandContextType::eAsyncCompute, 55, RHIAccessMode::eReadWrite);
    use.Add(RHICommandContextType::eGraphics, 66, RHIAccessMode::eReadWrite);
    RenderSubmissionUpdate rejected;
    ASSERT_TRUE(PrepareHistory(history, use, rejected));
    EXPECT_FALSE(history.Commit(rejected));
    ExpectPoint(history.Find(55)->writer.point, RHICommandContextType::eTransfer, 6);
    history.Erase(66); // Any external invalidation invalidates an already prepared delta.
    ASSERT_TRUE(RHISubmissionStateTestAccess::Queue(*rejected.state));
    EXPECT_FALSE(history.Commit(rejected));
    ExpectPoint(history.Find(55)->writer.point, RHICommandContextType::eTransfer, 6);
    RenderSubmissionUpdate accepted;
    ASSERT_TRUE(PrepareHistory(history, use, accepted));
    ASSERT_TRUE(RHISubmissionStateTestAccess::Queue(*accepted.state));
    ASSERT_TRUE(history.Commit(accepted));
    ASSERT_TRUE(RHISubmissionStateTestAccess::Accept(*accepted.state, 0,
                                                     {RHICommandContextType::eAsyncCompute, 7}));
    ASSERT_TRUE(history.ResolveAccepted());
    EXPECT_TRUE(
        history.Find(55)->writer.point.state); // Keep failure provenance until batch success.
    RenderSubmissionUpdate dependent;
    ASSERT_TRUE(PrepareHistory(history, input, dependent));
    RHISubmissionStateTestAccess::Fail(*accepted.state);
    EXPECT_FALSE(history.CanCommit(dependent));
    EXPECT_FALSE(history.ResolveAccepted());
    EXPECT_EQ(history.Find(55), nullptr);
    EXPECT_EQ(history.Find(66), nullptr);
}

TEST(RenderSubmissionHistoryTest, SameQueueReadersAccumulateScopesWithoutLosingPriorWriter)
{
    RenderSubmissionHistory history;
    HistoryInput input;
    input.Add(RHICommandContextType::eGraphics, 77, RHIAccessMode::eReadWrite);
    RenderSubmissionUpdate writer;
    ASSERT_TRUE(PrepareHistory(history, input, writer));
    const SmallVector<uint64_t, 1> first{3};
    AcceptHistory(history, writer, first);
    input.accesses[0][0].access.accessMode  = RHIAccessMode::eRead;
    input.accesses[0][0].access.accessFlags = int64_t(RHIAccessFlagBits::eShaderRead);
    RenderSubmissionUpdate reader;
    ASSERT_TRUE(PrepareHistory(history, input, reader));
    const SmallVector<uint64_t, 1> second{4};
    AcceptHistory(history, reader, second);
    input.accesses[0][0].access.pipelineStages = int64_t(RHIPipelineStageFlagBits::eVertexShader);
    ASSERT_TRUE(PrepareHistory(history, input, reader));
    const SmallVector<uint64_t, 1> third{5};
    AcceptHistory(history, reader, third);
    const RenderResourceHistory* state = history.Find(77);
    ASSERT_NE(state, nullptr);
    ExpectPoint(state->writer.point, RHICommandContextType::eGraphics, 3);
    ExpectPoint(state->readers[0].point, RHICommandContextType::eGraphics, 5);
    EXPECT_TRUE(
        state->readers[0].access.pipelineStages.HasFlag(RHIPipelineStageFlagBits::eVertexShader));
    EXPECT_TRUE(
        state->readers[0].access.pipelineStages.HasFlag(RHIPipelineStageFlagBits::eFragmentShader));
}

TEST(RenderSubmissionHistoryTest, CompatibleImageUsagesKeepEveryReaderStageForOverwrite)
{
    RenderSubmissionHistory history;
    HistoryInput input;
    input.Add(RHICommandContextType::eGraphics, 88, RHIAccessMode::eRead,
              RHITextureUsage::eSampled);
    input.accesses[0][0].access.pipelineStages = int64_t(RHIPipelineStageFlagBits::eVertexShader);
    input.Add(RHICommandContextType::eGraphics, 88, RHIAccessMode::eRead,
              RHITextureUsage::eInputAttachment);
    input.accesses[1][0].access.accessFlags = int64_t(RHIAccessFlagBits::eInputAttachmentRead);
    RenderSubmissionUpdate readers;
    ASSERT_TRUE(PrepareHistory(history, input, readers));
    const SmallVector<uint64_t, 2> serials{2, 3};
    AcceptHistory(history, readers, serials);
    const RenderResourceUse& reader = history.Find(88)->readers[0];
    ExpectPoint(reader.point, RHICommandContextType::eGraphics, 3);
    EXPECT_TRUE(reader.access.pipelineStages.HasFlag(RHIPipelineStageFlagBits::eVertexShader));
    EXPECT_TRUE(reader.access.pipelineStages.HasFlag(RHIPipelineStageFlagBits::eFragmentShader));
    EXPECT_TRUE(reader.access.accessFlags.HasFlag(RHIAccessFlagBits::eShaderRead));
    EXPECT_TRUE(reader.access.accessFlags.HasFlag(RHIAccessFlagBits::eInputAttachmentRead));
}

class RDGSubmissionHistoryTest : public RDGQueuePreferenceTest
{};

TEST_P(RDGSubmissionHistoryTest, UploadedResourceWaitIsAttachedOnlyToItsComputeConsumer)
{
    TestBuffer* uploaded  = Buffer();
    TestBuffer* unrelated = Buffer();
    const SmallVector<uint8_t, 64> bytes(64);
    device->UpdateBuffer(uploaded, uint32_t(bytes.size()), bytes.data());
    RenderGraph flush("flush_upload");
    ASSERT_TRUE(flush.Begin());
    ASSERT_TRUE(flush.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(flush));
    const RHISubmissionDependency producer = RDGSubmissionTestAccess::Submission(*device, uploaded);
    ASSERT_GT(producer.serial, 0u);
    RenderGraph graph("group_upload_history");
    ASSERT_TRUE(graph.Begin());
    AddScheduledBufferPass(graph, "independent", RHICommandContextType::eGraphics,
                           graph.GetResourceManager()->ImportHostWrittenBuffer(unrelated));
    AddScheduledBufferPass(graph, "consumer", RHICommandContextType::eAsyncCompute,
                           graph.GetResourceManager()->ImportBuffer(uploaded));
    ASSERT_TRUE(graph.End());
    RDGExecutor executor(device);
    executor.GetResourceStateTracker() = RDGSubmissionTestAccess::Tracker(*device);
    ConfigureGroupMetrics(executor);
    GroupAccess::Plan plan;
    ASSERT_TRUE(GroupAccess::Prepare(executor, graph, plan));
    RenderSubmissionUpdate update;
    const uint64_t queries = rhi->progressQueries;
    ASSERT_TRUE(RDGSubmissionTestAccess::PrepareHistory(*device, graph, plan.schedule, update));
    EXPECT_EQ(rhi->progressQueries, queries);
    ASSERT_EQ(update.schedule.groups.size(), 2u);
    EXPECT_TRUE(update.schedule.groups[0].externalPredecessors.empty());
    ASSERT_EQ(update.schedule.groups[1].externalPredecessors.size(), 1u);
    const uint32_t id = update.schedule.groups[1].externalPredecessors[0];
    ExpectPoint(update.externalPoints[id], producer.queue, producer.serial);
    plan.schedule = update.schedule;
    RecordedGroupLists recorded(*device, plan.schedule);
    ASSERT_TRUE(GroupAccess::ExecuteGroups(executor, plan, recorded.lists, update.initialStates));
    EXPECT_TRUE(plan.schedule.groups[0].externalPredecessors.empty());
    ExpectValidGroupMetrics(executor);
    EXPECT_TRUE(rhi->submissionWaits.empty());
    device->DestroyBuffer(uploaded);
    device->DestroyBuffer(unrelated);
}

TEST_P(RDGSubmissionHistoryTest, GroupRecordingMergesAllLocalReadersAndForeignWaits)
{
    TestBuffer* buffer = Buffer();
    HistoryInput input;
    input.Add(RHICommandContextType::eGraphics, buffer->GetStableId(), RHIAccessMode::eRead);
    input.Add(RHICommandContextType::eAsyncCompute, buffer->GetStableId(), RHIAccessMode::eRead);
    RenderSubmissionHistory& history = RDGSubmissionTestAccess::History(*device);
    RenderSubmissionUpdate readers;
    ASSERT_TRUE(PrepareHistory(history, input, readers));
    const SmallVector<uint64_t, 2> serials{8, 12};
    AcceptHistory(history, readers, serials);
    RenderGraph graph("overwrite_parallel_readers");
    ASSERT_TRUE(graph.Begin());
    AddScheduledBufferPass(graph, "overwrite", RHICommandContextType::eAsyncCompute, {},
                           graph.GetResourceManager()->ImportBuffer(buffer));
    ASSERT_TRUE(graph.End());
    RDGExecutor executor(device);
    ConfigureGroupMetrics(executor);
    GroupAccess::Plan plan;
    ASSERT_TRUE(GroupAccess::Prepare(executor, graph, plan));
    RenderSubmissionUpdate update;
    ASSERT_TRUE(RDGSubmissionTestAccess::PrepareHistory(*device, graph, plan.schedule, update));
    ASSERT_EQ(update.initialStates.size(), 2u);
    EXPECT_EQ(update.schedule.groups[0].externalPredecessors.size(), 2u);
    plan.schedule = update.schedule;
    RecordedGroupLists recorded(*device, plan.schedule);
    ASSERT_TRUE(GroupAccess::ExecuteGroups(executor, plan, recorded.lists, update.initialStates));
    ExpectValidGroupMetrics(executor);
    recorded.Replay();
    ASSERT_FALSE(rhi->compute.bufferTransitions.empty());
    EXPECT_EQ(rhi->compute.bufferTransitions[0].oldAccessMode, RHIAccessMode::eRead);
    for (const TestContext::BarrierBatch& batch : rhi->compute.barrierBatches)
    {
        EXPECT_TRUE(RHIQueueSupportsStages(rhi->computeCopy, batch.source));
    }
    device->InvalidateExternalBufferState(buffer);
    EXPECT_EQ(history.Find(buffer->GetStableId()), nullptr);
    EXPECT_FALSE(history.CanCommit(update));
    device->DestroyBuffer(buffer);
}

TEST_P(RDGSubmissionHistoryTest, TextureViewsSharePhysicalHistoryAndInvalidation)
{
    RHITexture* texture = Texture();
    RHITextureViewCreateInfo info;
    info.format          = texture->GetFormat();
    info.type            = RHITextureType::e2D;
    RHITextureView* view = texture->CreateView(info);
    ASSERT_NE(view, nullptr);
    RenderGraph graph("texture_view_history");
    ASSERT_TRUE(graph.Begin());
    RDGComputePassDesc pass = IntentPass();
    pass.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
    pass.BindStorageImage("write_image", view);
    graph.AddComputePass(pass);
    ASSERT_TRUE(graph.End());
    RDGExecutor executor(device);
    GroupAccess::Plan plan;
    ASSERT_TRUE(GroupAccess::Prepare(executor, graph, plan));
    RenderSubmissionUpdate update;
    ASSERT_TRUE(RDGSubmissionTestAccess::PrepareHistory(*device, graph, plan.schedule, update));
    RenderSubmissionHistory& history = RDGSubmissionTestAccess::History(*device);
    ASSERT_TRUE(RHISubmissionStateTestAccess::Queue(*update.state));
    ASSERT_TRUE(history.Commit(update));
    EXPECT_NE(history.Find(texture->GetStableId()), nullptr);
    EXPECT_EQ(history.Find(view->GetStableId()), nullptr);
    device->InvalidateExternalTextureState(texture);
    EXPECT_EQ(history.Find(texture->GetStableId()), nullptr);
    device->DestroyTexture(texture);
}

TEST_P(RDGSubmissionHistoryTest, ParallelImageReadersRetainLayoutAndWaitBeforeTransition)
{
    RHITexture* texture              = Texture();
    RenderSubmissionHistory& history = RDGSubmissionTestAccess::History(*device);
    HistoryInput input;
    input.Add(RHICommandContextType::eGraphics, texture->GetStableId(), RHIAccessMode::eRead,
              RHITextureUsage::eSampled);
    input.Add(RHICommandContextType::eAsyncCompute, texture->GetStableId(), RHIAccessMode::eRead,
              RHITextureUsage::eSampled);
    RenderSubmissionUpdate readers;
    ASSERT_TRUE(PrepareHistory(history, input, readers));
    const SmallVector<uint64_t, 2> serials{3, 5};
    AcceptHistory(history, readers, serials);

    for (bool transition : {false, true})
    {
        RenderGraph graph("prior_image_readers");
        ASSERT_TRUE(graph.Begin());
        const RDGTexture logical =
            graph.GetResourceManager()->ImportTexture(texture, RDGImportContents::eDefined);
        RDGComputePassDesc pass = IntentPass();
        pass.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
        if (transition)
        {
            pass.BindStorageImage("write_image", logical);
        }
        else
        {
            pass.BindSampledTexture("texture", nullptr, logical);
        }
        graph.AddComputePass(pass);
        ASSERT_TRUE(graph.End());
        RDGExecutor executor(device);
        ConfigureGroupMetrics(executor);
        GroupAccess::Plan plan;
        ASSERT_TRUE(GroupAccess::Prepare(executor, graph, plan));
        RenderSubmissionUpdate update;
        ASSERT_TRUE(RDGSubmissionTestAccess::PrepareHistory(*device, graph, plan.schedule, update));
        EXPECT_EQ(update.schedule.groups[0].externalPredecessors.size(), transition ? 2u : 1u);
        plan.schedule = update.schedule;
        RecordedGroupLists recorded(*device, plan.schedule);
        ASSERT_TRUE(
            GroupAccess::ExecuteGroups(executor, plan, recorded.lists, update.initialStates));
        ExpectValidGroupMetrics(executor);
        recorded.Replay();
        if (transition)
        {
            ASSERT_EQ(rhi->compute.textureTransitions.size(), 1u);
            EXPECT_EQ(rhi->compute.textureTransitions[0].oldUsage, RHITextureUsage::eSampled);
            EXPECT_EQ(rhi->compute.textureTransitions[0].newUsage, RHITextureUsage::eStorage);
            EXPECT_TRUE(rhi->compute.textureTransitions[0].GetSourceAccess().HasFlag(
                RHIAccessFlagBits::eShaderRead));
        }
        else
        {
            EXPECT_TRUE(rhi->compute.textureTransitions.empty());
        }
    }
    device->DestroyTexture(texture);
}

TEST_P(RDGGroupQueueTest, ExactReaderTimelinesShareLocalBarrierOnlyWhenNativeQueuesAlias)
{
    const bool alias   = std::get<1>(GetParam());
    TestBuffer* buffer = Buffer();
    HistoryInput input;
    input.Add(RHICommandContextType::eAsyncCompute, buffer->GetStableId(), RHIAccessMode::eRead);
    input.Add(RHICommandContextType::eTransfer, buffer->GetStableId(), RHIAccessMode::eRead);
    RenderSubmissionHistory& history = RDGSubmissionTestAccess::History(*device);
    RenderSubmissionUpdate readers;
    RHIQueueCapabilities queues = DistinctComputeQueues();
    queues.queueIds[2]          = alias ? 1 : 2;
    ASSERT_TRUE(PrepareHistory(history, input, readers, queues));
    const SmallVector<uint64_t, 2> serials{19, 2};
    AcceptHistory(history, readers, serials);
    RenderGraph graph("overwrite_aliased_readers");
    ASSERT_TRUE(graph.Begin());
    AddScheduledBufferPass(graph, "overwrite", RHICommandContextType::eAsyncCompute, {},
                           graph.GetResourceManager()->ImportBuffer(buffer));
    ASSERT_TRUE(graph.End());
    RDGExecutor executor(device);
    ConfigureGroupMetrics(executor);
    GroupAccess::Plan plan;
    ASSERT_TRUE(GroupAccess::Prepare(executor, graph, plan));
    RenderSubmissionUpdate update;
    ASSERT_TRUE(RDGSubmissionTestAccess::PrepareHistory(*device, graph, plan.schedule, update));
    EXPECT_EQ(update.schedule.groups[0].externalPredecessors.size(), 2u);
    plan.schedule = update.schedule;
    RecordedGroupLists recorded(*device, plan.schedule);
    ASSERT_TRUE(GroupAccess::ExecuteGroups(executor, plan, recorded.lists, update.initialStates));
    ExpectValidGroupMetrics(executor);
    recorded.Replay();
    ASSERT_FALSE(rhi->compute.bufferTransitions.empty());
    bool transferScope = false;
    for (const TestContext::BarrierBatch& batch : rhi->compute.barrierBatches)
    {
        transferScope |= batch.source.HasFlag(RHIPipelineStageFlagBits::eTransfer);
    }
    EXPECT_EQ(transferScope, alias);
    device->DestroyBuffer(buffer);
}

TEST_P(RDGSubmissionHistoryTest, LaterUploadWaitsForBothGraphicsAndComputeReaders)
{
    TestBuffer* buffer = Buffer();
    HistoryInput input;
    input.Add(RHICommandContextType::eGraphics, buffer->GetStableId(), RHIAccessMode::eRead);
    input.Add(RHICommandContextType::eAsyncCompute, buffer->GetStableId(), RHIAccessMode::eRead);
    RenderSubmissionHistory& history = RDGSubmissionTestAccess::History(*device);
    RenderSubmissionUpdate readers;
    ASSERT_TRUE(PrepareHistory(history, input, readers));
    const SmallVector<uint64_t, 2> serials{3, 7};
    AcceptHistory(history, readers, serials);
    GetRHIThread().Invoke([this] {
        rhi->submitted[0] = 3;
        rhi->submitted[1] = 7;
    });
    SmallVector<uint8_t, 64> bytes(64);
    std::fill(bytes.begin(), bytes.end(), uint8_t(0x6b));
    device->UpdateBuffer(buffer, uint32_t(bytes.size()), bytes.data());
    RenderGraph flush("overwrite_readers_upload");
    ASSERT_TRUE(flush.Begin());
    ASSERT_TRUE(flush.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(flush));
    bool graphics = false;
    bool compute  = false;
    for (const TestRHI::SubmissionDependency& wait : rhi->gpuDependencies)
    {
        graphics |= wait.consumer == RHICommandContextType::eTransfer &&
            wait.producer.queue == RHICommandContextType::eGraphics && wait.producer.serial == 3;
        compute |= wait.consumer == RHICommandContextType::eTransfer &&
            wait.producer.queue == RHICommandContextType::eAsyncCompute &&
            wait.producer.serial == 7;
    }
    EXPECT_TRUE(graphics);
    EXPECT_TRUE(compute);
    EXPECT_TRUE(rhi->submissionWaits.empty());
    EXPECT_EQ(std::memcmp(buffer->bytes.data(), bytes.data(), bytes.size()), 0);
    device->DestroyBuffer(buffer);
}

INSTANTIATE_TEST_SUITE_P(InlineAndThreaded,
                         RDGSubmissionHistoryTest,
                         testing::Values(RHIExecutionMode::eInline, RHIExecutionMode::eThreaded));

TEST_F(RHIExecutorTest, QueuedConsumerResolvesExactProducerBeforeUnrelatedSubmissions)
{
    rhi->submissionQueueCapabilities.asyncSubmissionDependencies = true;
    const RHICommandContextType producerQueue = RHICommandContextType::eAsyncCompute;
    RefCountPtr<RHISubmissionState> state =
        MakeRefCountPtr<RHISubmissionState>(MakeVecView(producerQueue));
    RHICommandListPtr producer(RHICommandList::Create(executor->GetCommandContext(producerQueue)));
    RHICommandListPtr consumer(
        RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));
    ArmGate();
    producer->Dispatch(1, 1, 1);
    const RHISubmissionTicket first = executor->SubmitFrame(*producer, nullptr, state);
    ASSERT_TRUE(first.IsValid());
    ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    RHISubmissionPoint point{producerQueue, 0, state, 0};
    RHISubmissionDependency resolved;
    EXPECT_EQ(point.Resolve(resolved), RHISubmissionPointStatus::ePending);
    // Queue another compute submission before the consumer; it must not change the point.
    producer->Dispatch(1, 1, 1);
    const RHISubmissionTicket unrelated = executor->SubmitFrame(*producer, nullptr);
    consumer->Dispatch(1, 1, 1);
    const RHISubmissionTicket last =
        executor->SubmitFrame(*consumer, nullptr, {}, MakeVecView(point));
    state.Reset();
    gate.Open();
    ASSERT_EQ(first.Wait().submission, RHISubmissionResult::eSuccess);
    ASSERT_EQ(unrelated.Wait().submission, RHISubmissionResult::eSuccess);
    ASSERT_EQ(last.Wait().submission, RHISubmissionResult::eSuccess);
    EXPECT_EQ(rhi->submitted[1], 2u);
    ExpectPoint(point, producerQueue, 1);
    ASSERT_EQ(rhi->gpuDependencies.size(), 1u);
    EXPECT_EQ(rhi->gpuDependencies[0].producer.serial, 1u);
    EXPECT_TRUE(rhi->submissionWaits.empty());
}

TEST_F(RHIExecutorTest, FailedExactProducerStopsQueuedConsumerBeforeNativeExecution)
{
    const RHICommandContextType queue     = RHICommandContextType::eAsyncCompute;
    RefCountPtr<RHISubmissionState> state = MakeRefCountPtr<RHISubmissionState>(MakeVecView(queue));
    RHICommandListPtr producer(RHICommandList::Create(executor->GetCommandContext(queue)));
    RHICommandListPtr consumer(
        RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));
    rhi->failSubmissionAt = 1;
    ArmGate();
    producer->Dispatch(1, 1, 1);
    const RHISubmissionTicket first = executor->SubmitFrame(*producer, nullptr, state);
    ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const RHISubmissionPoint point{queue, 0, state, 0};
    consumer->Draw(3, 1, 0, 0);
    const RHISubmissionTicket second =
        executor->SubmitFrame(*consumer, nullptr, {}, MakeVecView(point));
    ASSERT_TRUE(second.IsValid());
    gate.Open();
    EXPECT_EQ(first.Wait().submission, RHISubmissionResult::eRejected);
    EXPECT_EQ(second.Wait().submission, RHISubmissionResult::eFatal);
    RHISubmissionDependency resolved;
    EXPECT_EQ(point.Resolve(resolved), RHISubmissionPointStatus::eFailed);
    EXPECT_TRUE(executor->AreSubmissionsBlocked());
    EXPECT_EQ(rhi->submissionAttempts, 1u);
    EXPECT_EQ(rhi->graphics.drawCount, 0u);
}

TEST_F(ThreadedRenderCoreTest, ExternalInvalidationSurvivesPendingFrameConfirmation)
{
    CreateTestShaderProgram(device, "intent");
    TestBuffer* buffer = Buffer();
    RenderGraph* graph = device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph->Begin());
    RDGComputePassDesc pass = IntentPass();
    pass.BindStorageBuffer("write_buffer", buffer, RDGContentGuarantee::eFullWrite);
    graph->AddComputePass(pass).RecordPassCommands(
        [](RDGPassCmdEncoder& encoder) { encoder.Dispatch(1, 1, 1); });
    ASSERT_TRUE(graph->End());
    ArmGate();
    ASSERT_TRUE(device->ExecuteRenderGraph(&viewport));
    ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_EQ(RDGSubmissionTestAccess::PendingFrameCount(*device), 1u);
    EXPECT_NE(RDGSubmissionTestAccess::History(*device).Find(buffer->GetStableId()), nullptr);
    device->InvalidateExternalBufferState(buffer);
    EXPECT_FALSE(RDGSubmissionTestAccess::HasHistory(*device, buffer->GetStableId()));
    gate.Open();
    device->FlushRHIThread();
    EXPECT_FALSE(RDGSubmissionTestAccess::HasHistory(*device, buffer->GetStableId()));
    device->DestroyBuffer(buffer);
}
} // namespace
