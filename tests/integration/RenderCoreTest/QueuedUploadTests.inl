// Included after the shared RenderCore, RHI executor and upload fixtures.
namespace
{
class QueuedUploadTest : public RenderCoreTest, public testing::WithParamInterface<bool>
{
protected:
    void SetUp() override
    {
        const uint32_t transferQueue = GetParam() ? 0u : 2u;

        InitializeDevice(nullptr, 3, RHIExecutionMode::eThreaded, AsyncComputeMode::eDisabled,
                         {false, true, {0, 1, transferQueue}});
    }

    void TearDown() override
    {
        gate.Open();

        viewport.ReleaseForTeardown();

        RenderCoreTest::TearDown();
    }

    void ArmGate()
    {
        device->FlushRHIThread();

        rhi->beforeSubmission = std::bind_front(&RHISubmissionGate::OnSubmission, &gate);
    }

    TestViewport      viewport;
    RHISubmissionGate gate;
};

TEST_P(QueuedUploadTest, NextFrameUploadAndHandoffDoNotWaitForPriorFrame)
{
    TestBuffer*              source = Buffer();
    TestBuffer*              first  = Buffer();
    TestBuffer*              second = Buffer();
    SmallVector<uint8_t, 64> bytes(64);
    std::fill(bytes.begin(), bytes.end(), 19);

    // Warm staging storage before the gate: cold native allocation is a separate cost.
    ASSERT_TRUE(device->UpdateBuffer(source, bytes.size(), bytes.data()));
    RenderGraph warmup("upload_warmup");
    ASSERT_TRUE(warmup.Begin());
    ASSERT_TRUE(warmup.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(warmup));
    device->WaitForPreviousFrames();

    RenderGraph& graph = *device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph.Begin());
    graph.AddTransferPass("first_frame_copy").CopyBuffer(source, first, {0, 0, 64});
    ASSERT_TRUE(graph.End());
    ArmGate();
    const uint64_t frameWait = device->GetFrameSubmissionWaitNS();
    const uint64_t groupWait = device->GetRHIThreadMetrics().submitGroupsWaitNS;
    ASSERT_EQ(device->ExecuteRenderGraph(&viewport), RenderFrameResult::eSubmitted);
    ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);

    device->NextFrame();
    std::fill(bytes.begin(), bytes.end(), 73);
    ASSERT_TRUE(device->UpdateBuffer(source, bytes.size(), bytes.data()));
    ASSERT_TRUE(graph.Begin());
    graph.AddTransferPass("second_frame_copy").CopyBuffer(source, second, {0, 0, 64});
    ASSERT_TRUE(graph.End());
    ASSERT_EQ(device->ExecuteRenderGraph(&viewport), RenderFrameResult::eSubmitted);

    EXPECT_EQ(RDGSubmissionTestAccess::PendingFrameCount(*device), 2u);
    ResourceRetirement retirement = device->CaptureResourceRetirement();
    EXPECT_EQ(retirement.pending.size(), 3u);
    EXPECT_FALSE(retirement.IsCompleteAt({{1000, 1000, 1000}}));
    EXPECT_EQ(device->GetFrameSubmissionWaitNS(), frameWait);
    EXPECT_EQ(device->GetRHIThreadMetrics().submitGroupsWaitNS, groupWait);

    gate.Open();
    device->FlushRHIThread();
    EXPECT_TRUE(gate.releasedInTime);
    EXPECT_EQ(RDGSubmissionTestAccess::PendingFrameCount(*device), 0u);
    EXPECT_TRUE(std::all_of(first->bytes.begin(), first->bytes.end(), [](uint8_t value) { return value == 19; }));
    EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(), second->bytes.begin()));
    // Warm-up upload -> frame N, frame N -> new upload, new upload -> frame N+1.
    EXPECT_EQ(rhi->gpuDependencies.size(), GetParam() ? 0u : 3u);
    EXPECT_TRUE(rhi->submissionWaits.empty());
    EXPECT_TRUE(retirement.Resolve());
    EXPECT_FALSE(device->IsResourceRetired(retirement));

    device->WaitForPreviousFrames();
    EXPECT_TRUE(device->IsResourceRetired(retirement));
    device->DestroyBuffer(source);
    device->DestroyBuffer(first);
    device->DestroyBuffer(second);
}

TEST_P(QueuedUploadTest, CompletedUploadTicketDoesNotDrainNewerFrameWhenSlotIsReused)
{
    SmallVector<uint8_t, 64> bytes(64);

    TestBuffer* source = Buffer();

    TestBuffer* output = Buffer();

    ASSERT_TRUE(device->UpdateBuffer(source, bytes.size(), bytes.data()));

    RenderGraph& graph = *device->GetCurrentFrameRDG();

    for (uint32_t frame = 0; frame < 2; ++frame)
    {
        ASSERT_TRUE(graph.Begin());

        graph.AddTransferPass("warm_slot").CopyBuffer(source, output, {0, 0, 64});

        ASSERT_TRUE(graph.End());

        ASSERT_EQ(device->ExecuteRenderGraph(&viewport), RenderFrameResult::eSubmitted);

        device->FlushRHIThread();

        device->NextFrame();
    }

    device->WaitForPreviousFrames();

    ArmGate();

    ASSERT_TRUE(graph.Begin());

    graph.AddTransferPass("later_frame").CopyBuffer(source, output, {0, 0, 64});

    ASSERT_TRUE(graph.End());

    ASSERT_EQ(device->ExecuteRenderGraph(&viewport), RenderFrameResult::eSubmitted);

    ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);

    const uint64_t waitBefore = device->GetFrameSubmissionWaitNS();

    device->NextFrame();

    EXPECT_EQ(ToIndex(GRenderFrameState.GetFrameSlot()), 0u);

    EXPECT_EQ(RDGSubmissionTestAccess::PendingFrameCount(*device), 1u);

    EXPECT_EQ(device->GetFrameSubmissionWaitNS(), waitBefore);

    gate.Open();

    device->FlushRHIThread();

    EXPECT_TRUE(gate.releasedInTime);

    device->DestroyBuffer(source);

    device->DestroyBuffer(output);
}

TEST_P(QueuedUploadTest, LateUploadRejectionBlocksAlreadyQueuedConsumerAndRetainsOwners)
{
    StagingBufferManager     staging(64, 64);
    StagingUploadQueue       uploads(device, &staging);
    TestBuffer*              source   = Buffer();
    TestBuffer*              output   = Buffer();
    const uint64_t           sourceId = source->GetStableId();
    SmallVector<uint8_t, 64> bytes(64);
    std::fill(bytes.begin(), bytes.end(), 37);
    // Create the graphics recording context before holding the RHI worker.
    RenderGraph& graph = *device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph.Begin());
    graph.AddTransferPass("warm_graphics_context").CopyBuffer(output, source, {0, 0, 64});
    ASSERT_TRUE(graph.End());
    ASSERT_EQ(device->ExecuteRenderGraph(&viewport), RenderFrameResult::eSubmitted);
    device->FlushRHIThread();
    device->NextFrame();
    ASSERT_TRUE(uploads.EnqueueBuffer(source, 0, bytes.size(), bytes.data()));
    ArmGate();
    const uint32_t attempt          = rhi->submissionAttempts + 1;
    const uint64_t completedBatches = device->GetRHIThreadMetrics().completedBatches;
    rhi->failSubmissionAt           = attempt;
    ASSERT_TRUE(uploads.Flush());
    ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_TRUE(graph.Begin());
    graph.AddTransferPass("failed_upload_consumer").CopyBuffer(source, output, {0, 0, 64});
    ASSERT_TRUE(graph.End());
    ASSERT_EQ(device->ExecuteRenderGraph(&viewport), RenderFrameResult::eSubmitted);
    device->DestroyBuffer(source);
    gate.Open();
    device->FlushRHIThread();

    EXPECT_TRUE(gate.releasedInTime);
    EXPECT_TRUE(device->AreSubmissionsBlocked());
    EXPECT_EQ(rhi->submissionAttempts, attempt);
    EXPECT_EQ(device->GetRHIThreadMetrics().completedBatches, completedBatches + 2u);
    EXPECT_TRUE(std::all_of(output->bytes.begin(), output->bytes.end(), [](uint8_t value) { return value == 0xCD; }));
    EXPECT_FALSE(destroyed.contains(sourceId));
    StagingAllocation reuse;
    EXPECT_EQ(staging.Allocate(64, 4, &reuse), StagingFlushAction::eFlush);
    uploads.Destroy();
    staging.Destroy();
    device->CollectCompletedResources();
    device->FlushRHIThread();
    EXPECT_FALSE(destroyed.contains(sourceId));
    device->DestroyBuffer(output);
}

TEST_P(AsyncUploadTest, DestroyDoesNotFlushPendingBufferOrTextureUploads)
{
    SmallVector<uint8_t, 16> bytes(16);
    std::fill(bytes.begin(), bytes.end(), 53);
    TestBuffer*                buffer    = static_cast<TestBuffer*>(device->CreateVertexBuffer(bytes.size(), bytes.data()));
    RHITexture*                texture   = Texture();
    const uint64_t             bufferId  = buffer->GetStableId();
    const uint64_t             textureId = texture->GetStableId();
    RHIBufferTextureCopyRegion region;
    region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    region.textureSize = {2, 2, 1};
    device->UpdateTexture(texture, MakeVecView(&region, 1), bytes.size(), bytes.data());
    device->DestroyBuffer(buffer);
    device->DestroyTexture(texture);
    EXPECT_EQ(device->GetRHIThreadMetrics().submittedBatches, 0u);
    EXPECT_FALSE(destroyed.contains(bufferId));
    EXPECT_FALSE(destroyed.contains(textureId));

    RenderGraph graph("flush_destroyed_uploads");
    ASSERT_TRUE(graph.Begin());
    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    device->FlushRHIThread();
    EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(), buffer->bytes.begin()));
    EXPECT_EQ(rhi->transfer.textureCopies.size(), 1u);
    EXPECT_FALSE(destroyed.contains(bufferId));
    EXPECT_FALSE(destroyed.contains(textureId));
    device->WaitForPreviousFrames();
    device->NextFrame();
    device->CollectCompletedResources();
    device->FlushRHIThread();
    EXPECT_TRUE(destroyed.contains(bufferId));
    EXPECT_TRUE(destroyed.contains(textureId));
}

TEST_P(QueuedUploadTest, PartialUploadFailureKeepsStagingAndDestinationUntilTeardown)
{
    StagingBufferManager     staging(64, 64);
    StagingUploadQueue       uploads(device, &staging);
    TestBuffer*              buffer = Buffer();
    const uint64_t           id     = buffer->GetStableId();
    SmallVector<uint8_t, 64> bytes(64);
    std::fill(bytes.begin(), bytes.end(), 61);
    ASSERT_TRUE(uploads.EnqueueBuffer(buffer, 0, bytes.size(), bytes.data()));
    ArmGate();
    rhi->failSubmissionAt    = 1;
    rhi->submitBeforeFailure = true;
    ASSERT_TRUE(uploads.Flush());
    ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ResourceRetirement retirement = device->CaptureResourceRetirement();
    gate.Open();
    device->FlushRHIThread();
    EXPECT_TRUE(device->AreSubmissionsBlocked());
    EXPECT_FALSE(retirement.Resolve());
    EXPECT_EQ(retirement.requiredSerials.Get(RHICommandContextType::eTransfer), 1u);
    EXPECT_FALSE(retirement.IsCompleteAt({{1000, 1000, 1000}}));
    StagingAllocation reuse;
    EXPECT_EQ(staging.Allocate(64, 4, &reuse), StagingFlushAction::eFlush);
    device->DestroyBuffer(buffer);
    uploads.Destroy();
    staging.Destroy();
    device->CollectCompletedResources();
    device->FlushRHIThread();
    EXPECT_FALSE(destroyed.contains(id));
}

TEST_F(ThreadedRenderCoreTest, RejectedUploadWithoutTimelineRetainsPayloadForSynchronousRetry)
{
    StagingBufferManager     staging(64, 64);
    StagingUploadQueue       uploads(device, &staging);
    TestBuffer*              buffer = Buffer();
    SmallVector<uint8_t, 64> bytes(64);
    std::fill(bytes.begin(), bytes.end(), 37);
    ASSERT_TRUE(uploads.EnqueueBuffer(buffer, 0, bytes.size(), bytes.data()));
    rhi->failSubmissionAt = 1;
    EXPECT_FALSE(uploads.Flush());
    EXPECT_TRUE(uploads.HasPending());
    EXPECT_FALSE(device->AreSubmissionsBlocked());
    EXPECT_TRUE(std::all_of(buffer->bytes.begin(), buffer->bytes.end(), [](uint8_t value) { return value == 0xCD; }));

    std::fill(bytes.begin(), bytes.end(), 91);
    rhi->failSubmissionAt = 0;
    ASSERT_TRUE(uploads.Flush());
    EXPECT_FALSE(uploads.HasPending());
    EXPECT_TRUE(std::all_of(buffer->bytes.begin(), buffer->bytes.end(), [](uint8_t value) { return value == 37; }));
    EXPECT_EQ(rhi->completed[2], rhi->submitted[2]);
    EXPECT_GT(rhi->completed[2], 0u);
    EXPECT_FALSE(rhi->submissionWaits.empty());
    uploads.Destroy();
    staging.Destroy();
    device->DestroyBuffer(buffer);
}

TEST_F(RHIExecutorTest, StagingBlockWaitsForEveryQueuedBatchAndUsesOnlyTheirOwnSerials)
{
    StagingBufferManager staging(32, 32);
    StagingAllocation    first, second, reuse;
    ASSERT_EQ(staging.Allocate(16, 4, &first), StagingFlushAction::eNone);
    ASSERT_EQ(staging.Allocate(16, 4, &second), StagingFlushAction::eNone);
    RHICommandListPtr transfer(RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eTransfer)));
    RHICommandListPtr compute(RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eAsyncCompute)));
    transfer->ClearBuffer(first.pBuffer, first.offset, first.size);
    compute->ClearBuffer(second.pBuffer, second.offset, second.size);
    const RHISubmissionGroup transferGroup{transfer.get()};
    const RHISubmissionGroup computeGroup{compute.get()};
    ArmGate();
    ResourceRetirement transferRetirement;
    transferRetirement.pending.push_back(executor->SubmitQueuedGroups(MakeVecView(transferGroup), {}));
    ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ResourceRetirement computeRetirement;
    computeRetirement.pending.push_back(executor->SubmitQueuedGroups(MakeVecView(computeGroup), {}));
    staging.Release(first, transferRetirement);
    staging.Release(second, computeRetirement);
    EXPECT_EQ(staging.Allocate(32, 4, &reuse), StagingFlushAction::eFlush);
    gate.Open();
    executor->FlushRHIThread();
    ASSERT_TRUE(transferRetirement.Resolve());
    ASSERT_TRUE(computeRetirement.Resolve());
    for (RHICommandContextType queue :
         {RHICommandContextType::eGraphics, RHICommandContextType::eAsyncCompute, RHICommandContextType::eTransfer})
    {
        EXPECT_EQ(transferRetirement.requiredSerials.Get(queue), queue == RHICommandContextType::eTransfer ? 1u : 0u);
        EXPECT_EQ(computeRetirement.requiredSerials.Get(queue), queue == RHICommandContextType::eAsyncCompute ? 1u : 0u);
    }
    EXPECT_EQ(ToValue(GRHIFrameState.GetFrameNumber()), 0u);

    ASSERT_TRUE(executor->WaitForCompletion(RHICommandContextType::eAsyncCompute, 1));
    EXPECT_EQ(staging.Allocate(32, 4, &reuse), StagingFlushAction::eFlush);
    ASSERT_TRUE(executor->WaitForCompletion(RHICommandContextType::eTransfer, 1));
    ASSERT_EQ(staging.Allocate(32, 4, &reuse), StagingFlushAction::eNone);
    EXPECT_EQ(reuse.pBuffer, first.pBuffer);
    EXPECT_EQ(reuse.offset, 0u);
    staging.Release(reuse, {});
    staging.Destroy();
}

INSTANTIATE_TEST_SUITE_P(SharedTransferQueue, QueuedUploadTest, testing::Bool());
} // namespace
