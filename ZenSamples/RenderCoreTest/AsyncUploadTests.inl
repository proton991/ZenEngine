// Included by RenderCoreTests.cpp to use the same fake backend and resource fixtures.
namespace
{
class AsyncUploadTest : public RenderCoreTest, public testing::WithParamInterface<RHIExecutionMode>
{
protected:
    TestViewport viewport;

    void SetUp() override
    {
        InitializeDevice(nullptr, 3, GetParam(), true);
        CreateTestShaderProgram(device, "intent");
    }

    bool ReadOnGraphics(TestBuffer* buffer)
    {
        RenderGraph graph("upload_consumer");
        graph.Begin();
        RDGComputePassDesc pass = IntentPass("read_upload");
        pass.BindStorageBuffer("read_buffer", graph.GetResourceManager()->ImportBuffer(buffer));
        graph.AddComputePass(pass);
        return graph.End() && device->ExecuteRenderGraph(graph);
    }
};

TEST_P(AsyncUploadTest, UploadStaysInFlightAndOnlyItsConsumerWaits)
{
    TestBuffer* uploaded  = Buffer();
    TestBuffer* unrelated = Buffer();
    StagingBufferManager staging(64, 128);
    StagingUploadQueue uploads(device, &staging);
    const std::array<uint8_t, 64> bytes{};
    uploads.EnqueueBuffer(uploaded, 0, bytes.size(), bytes.data());
    ASSERT_TRUE(uploads.Flush());
    const uint64_t serial = RDGSubmissionTestAccess::Submission(*device, uploaded).serial;
    EXPECT_GT(serial, 0u);
    EXPECT_EQ(rhi->completed[2], 0u);
    EXPECT_TRUE(rhi->submissionWaits.empty());
    ASSERT_TRUE(ReadOnGraphics(unrelated));
    EXPECT_TRUE(rhi->gpuDependencies.empty());
    ASSERT_TRUE(ReadOnGraphics(uploaded));
    ASSERT_EQ(rhi->gpuDependencies.size(), 1u);
    EXPECT_EQ(rhi->gpuDependencies[0].consumer, RHICommandContextType::eGraphics);
    EXPECT_EQ(rhi->gpuDependencies[0].producer.queue, RHICommandContextType::eTransfer);
    EXPECT_EQ(rhi->gpuDependencies[0].producer.serial, serial);
    EXPECT_EQ(rhi->completed[2], 0u);
    EXPECT_TRUE(rhi->submissionWaits.empty());
    uploads.Destroy();
    staging.Destroy();
    device->DestroyBuffer(uploaded);
    device->DestroyBuffer(unrelated);
}

TEST_P(AsyncUploadTest, GraphicsCopyToTransferWaitsForPriorGraphicsUse)
{
    TestBuffer* input        = Buffer();
    TestBuffer* intermediate = Buffer();
    TestBuffer* output       = Buffer();
    ASSERT_TRUE(ReadOnGraphics(intermediate));
    RenderGraph graphics("graphics_copy");
    graphics.Begin();
    graphics.AddTransferPass("copy").CopyBuffer(input, intermediate, {0, 0, 64});
    ASSERT_TRUE(graphics.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graphics));
    const uint64_t graphicsSerial = rhi->submitted[0];
    EXPECT_EQ(rhi->submitted[2], 0u);

    RenderGraph transfer("transfer_after_graphics");
    transfer.Begin();
    transfer.AddTransferPass("copy").CopyBuffer(intermediate, output, {0, 0, 64});
    ASSERT_TRUE(transfer.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(transfer));
    ASSERT_EQ(rhi->gpuDependencies.size(), 1u);
    EXPECT_EQ(rhi->gpuDependencies[0].consumer, RHICommandContextType::eTransfer);
    EXPECT_EQ(rhi->gpuDependencies[0].producer.queue, RHICommandContextType::eGraphics);
    EXPECT_EQ(rhi->gpuDependencies[0].producer.serial, graphicsSerial);
    EXPECT_TRUE(rhi->submissionWaits.empty());
    device->DestroyBuffer(input);
    device->DestroyBuffer(intermediate);
    device->DestroyBuffer(output);
}

TEST_P(AsyncUploadTest, RejectedConsumerPreservesUploadDependencyForRetry)
{
    TestBuffer* buffer = Buffer();
    const std::array<uint8_t, 64> bytes{};
    device->UpdateBuffer(buffer, bytes.size(), bytes.data());
    RenderGraph uploadFlush("flush_upload");
    uploadFlush.Begin();
    ASSERT_TRUE(uploadFlush.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(uploadFlush));
    const uint64_t serial = RDGSubmissionTestAccess::Submission(*device, buffer).serial;
    rhi->failSubmissionAt = rhi->submissionAttempts + 1;
    EXPECT_FALSE(ReadOnGraphics(buffer));
    rhi->failSubmissionAt = 0;
    ASSERT_TRUE(ReadOnGraphics(buffer));
    ASSERT_EQ(rhi->gpuDependencies.size(), 2u);
    EXPECT_EQ(rhi->gpuDependencies[0].producer.serial, serial);
    EXPECT_EQ(rhi->gpuDependencies[1].producer.serial, serial);
    EXPECT_TRUE(rhi->submissionWaits.empty());
    device->DestroyBuffer(buffer);
}

TEST_P(AsyncUploadTest, StagingAllocationCannotBeReusedBeforeTransferCompletion)
{
    TestBuffer* buffer = Buffer();
    StagingBufferManager staging(64, 64);
    StagingUploadQueue uploads(device, &staging);
    const std::array<uint8_t, 64> bytes{};
    uploads.EnqueueBuffer(buffer, 0, bytes.size(), bytes.data());
    ASSERT_TRUE(uploads.Flush());
    uploads.ReclaimResources();
    StagingAllocation allocation;
    EXPECT_NE(staging.Allocate(64, 4, &allocation), StagingFlushAction::eNone);
    ASSERT_TRUE(
        GDynamicRHI->WaitForSubmission(RHICommandContextType::eTransfer, rhi->submitted[2]));
    uploads.ReclaimResources();
    ASSERT_EQ(staging.Allocate(64, 4, &allocation), StagingFlushAction::eNone);
    staging.Release(allocation, {});
    uploads.Destroy();
    staging.Destroy();
    device->DestroyBuffer(buffer);
}

TEST_P(AsyncUploadTest, MipmapUploadKeepsGraphicsFallbackWithoutCompletionWait)
{
    asset::TextureInfo input{};
    input.width = input.height = 2;
    input.data.resize(16, 81);
    textureFiles["async_mips.png"] = input;
    StagingBufferManager staging(1024, 4096);
    StagingUploadQueue uploads(device, &staging);
    TextureManager textures(device, &uploads);
    RHITexture* texture = textures.LoadTexture2D("async_mips.png", true);
    ASSERT_NE(texture, nullptr);
    ASSERT_TRUE(uploads.Flush());
    EXPECT_EQ(RDGSubmissionTestAccess::Submission(*device, texture).queue,
              RHICommandContextType::eGraphics);
    EXPECT_EQ(rhi->submitted[2], 0u);
    EXPECT_GT(rhi->submitted[0], 0u);
    EXPECT_EQ(rhi->completed[0], 0u);
    EXPECT_TRUE(rhi->submissionWaits.empty());
    textures.Destroy();
    uploads.Destroy();
    staging.Destroy();
}

TEST_P(AsyncUploadTest, RecordedDependenciesSurviveDetachAndRollback)
{
    RHICommandListPtr commands(
        RHICommandList::Create(GDynamicRHI->GetCommandContext(RHICommandContextType::eGraphics)));
    commands->AddSubmissionDependency({RHICommandContextType::eTransfer, 2});
    const RHICommandListBase::CommandCheckpoint checkpoint = commands->GetCommandCheckpoint();
    commands->AddSubmissionDependency({RHICommandContextType::eTransfer, 5});
    commands->RollbackCommands(checkpoint);
    RHICommandListPtr detached = commands->DetachCommands();
    EXPECT_TRUE(commands->GetSubmissionDependencies().empty());
    ASSERT_EQ(detached->GetSubmissionDependencies().size(), 1u);
    EXPECT_EQ(detached->GetSubmissionDependencies()[0].serial, 2u);
    detached->Reset();
    EXPECT_TRUE(detached->GetSubmissionDependencies().empty());
}

TEST_P(AsyncUploadTest, RecordingGraphLeavesSubmissionDependenciesToRenderDevice)
{
    TestBuffer* uploaded = Buffer();
    StagingBufferManager staging(64, 128);
    StagingUploadQueue uploads(device, &staging);
    const std::array<uint8_t, 64> bytes{};
    uploads.EnqueueBuffer(uploaded, 0, bytes.size(), bytes.data());
    ASSERT_TRUE(uploads.Flush());
    const RHISubmissionDependency producer = RDGSubmissionTestAccess::Submission(*device, uploaded);
    const uint32_t attempts                = rhi->submissionAttempts;

    RenderGraph graph("record_upload_consumer");
    ASSERT_TRUE(graph.Begin());
    RDGComputePassDesc pass = IntentPass("read_upload");
    pass.BindStorageBuffer("read_buffer", graph.GetResourceManager()->ImportBuffer(uploaded));
    graph.AddComputePass(pass);
    ASSERT_TRUE(graph.End());
    RDGExecutor recorder(device);
    recorder.GetResourceStateTracker() = RDGSubmissionTestAccess::Tracker(*device);
    RHICommandListPtr commands(
        RHICommandList::Create(GDynamicRHI->GetCommandContext(RHICommandContextType::eGraphics)));
    ASSERT_TRUE(recorder.Execute(&graph, commands.get()));
    EXPECT_TRUE(commands->GetSubmissionDependencies().empty());
    EXPECT_EQ(rhi->submissionAttempts, attempts);
    EXPECT_TRUE(rhi->gpuDependencies.empty());
    EXPECT_EQ(RDGSubmissionTestAccess::Submission(*device, uploaded).queue, producer.queue);
    EXPECT_EQ(RDGSubmissionTestAccess::Submission(*device, uploaded).serial, producer.serial);
    commands->Reset();

    ASSERT_TRUE(ReadOnGraphics(uploaded));
    ASSERT_EQ(rhi->gpuDependencies.size(), 1u);
    EXPECT_EQ(rhi->gpuDependencies[0].producer.serial, producer.serial);
    uploads.Destroy();
    staging.Destroy();
    device->DestroyBuffer(uploaded);
}

TEST_P(AsyncUploadTest, FrameSubmissionKeepsUploadWaitAndOrdersLaterTransfer)
{
    TestBuffer* source       = Buffer();
    TestBuffer* intermediate = Buffer();
    TestBuffer* output       = Buffer();
    const std::array<uint8_t, 64> bytes{};
    device->UpdateBuffer(source, bytes.size(), bytes.data());
    RenderGraph* frame = device->GetCurrentFrameRDG();
    ASSERT_TRUE(frame->Begin());
    frame->AddTransferPass("frame_copy").CopyBuffer(source, intermediate, {0, 0, 64});
    ASSERT_TRUE(frame->End());
    ASSERT_TRUE(device->ExecuteRenderGraph(&viewport));
    device->FlushRHIThread();
    ASSERT_EQ(rhi->gpuDependencies.size(), 1u);
    EXPECT_EQ(rhi->gpuDependencies[0].consumer, RHICommandContextType::eGraphics);
    EXPECT_EQ(rhi->gpuDependencies[0].producer.queue, RHICommandContextType::eTransfer);
    const uint64_t graphicsSerial = rhi->submitted[0];

    RenderGraph transfer("read_frame_copy");
    ASSERT_TRUE(transfer.Begin());
    transfer.AddTransferPass("copy").CopyBuffer(intermediate, output, {0, 0, 64});
    ASSERT_TRUE(transfer.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(transfer));
    ASSERT_EQ(rhi->gpuDependencies.size(), 2u);
    EXPECT_EQ(rhi->gpuDependencies[1].consumer, RHICommandContextType::eTransfer);
    EXPECT_EQ(rhi->gpuDependencies[1].producer.queue, RHICommandContextType::eGraphics);
    // Queued frames resolve their serial on RHI after all earlier frame work is submitted.
    const uint64_t expected =
        GetParam() == RHIExecutionMode::eThreaded ? graphicsSerial : graphicsSerial - 1;
    EXPECT_EQ(rhi->gpuDependencies[1].producer.serial, expected);
    EXPECT_TRUE(rhi->submissionWaits.empty());
    device->DestroyBuffer(source);
    device->DestroyBuffer(intermediate);
    device->DestroyBuffer(output);
}

INSTANTIATE_TEST_SUITE_P(ExecutionModes,
                         AsyncUploadTest,
                         testing::Values(RHIExecutionMode::eInline, RHIExecutionMode::eThreaded));
} // namespace
