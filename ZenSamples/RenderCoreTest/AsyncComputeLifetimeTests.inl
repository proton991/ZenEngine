// Included after the shared RenderCore and RHI executor fixtures.
namespace
{
class AsyncComputeLifetimeTest :
    public RenderCoreTest,
    public testing::WithParamInterface<RHIExecutionMode>
{
protected:
    void SetUp() override
    {
        InitializeDevice(nullptr, 2, GetParam(), true);
    }

    void CompleteGraphicsAndTransfer()
    {
        ASSERT_TRUE(GDynamicRHI->WaitForSubmission(
            RHICommandContextType::eGraphics,
            GDynamicRHI->GetLastSubmittedSerial(RHICommandContextType::eGraphics)));
        ASSERT_TRUE(GDynamicRHI->WaitForSubmission(
            RHICommandContextType::eTransfer,
            GDynamicRHI->GetLastSubmittedSerial(RHICommandContextType::eTransfer)));
        device->FlushRHIThread();
        device->CollectCompletedResources();
    }

    RHIBatchResult SubmitCompute(RHIResource* resource)
    {
        RHICommandListPtr commands(RHICommandList::Create(
            GDynamicRHI->GetCommandContext(RHICommandContextType::eAsyncCompute)));
        commands->RetainResource(resource);
        RHICommandListExecutor* executor = static_cast<RHICommandListExecutor*>(GDynamicRHI);
        const RHIBatchResult result      = executor->SubmitFrame(*commands, nullptr).Wait();
        device->FlushRHIThread();
        EXPECT_EQ(result.submission, RHISubmissionResult::eSuccess);
        EXPECT_GT(result.completion.Get(RHICommandContextType::eAsyncCompute), 0u);
        return result;
    }
};

TEST(RHICompletionSetTest, ComparesOnlyCorrespondingQueueTimelines)
{
    RHICompletionSet required{{100, 5, 9}};
    required.Extend(RHICommandContextType::eAsyncCompute, 3);
    required.Extend(RHICompletionSet{{99, 7, 8}});
    EXPECT_EQ(required.Get(RHICommandContextType::eGraphics), 100u);
    EXPECT_EQ(required.Get(RHICommandContextType::eAsyncCompute), 7u);
    EXPECT_FALSE(required.IsCompleteAt(RHICompletionSet{{1000, 6, 1000}}));
    EXPECT_TRUE(required.IsCompleteAt(RHICompletionSet{{100, 7, 9}}));
    required.Reset();
    EXPECT_TRUE(required.IsCompleteAt({}));
}

TEST_P(AsyncComputeLifetimeTest, ComputeWithoutGraphicsConsumerProtectsPoolAndRetiredBytes)
{
    RenderGraph graph("compute_pool_lifetime");
    RDGResourceManager* resources = graph.GetResourceManager();
    ASSERT_TRUE(graph.Begin());
    const RDGTexture texture = resources->CreateTexture(LogicalTexture());
    graph.AddTransferPass("clear").NeverCull().ClearTexture(texture, Color(0.f));
    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    const uint64_t id = DescribeResource(resources, texture).physicalStableId;
    // This step tests retirement through RHI directly; graph queue scheduling is added later.
    const RHIBatchResult compute = SubmitCompute(rhi->lastCreatedTexture);
    ASSERT_TRUE(graph.Reset());
    CompleteGraphicsAndTransfer();
    EXPECT_GT(resources->GetPoolStats().inFlightBytes, 0u);
    ASSERT_TRUE(graph.Begin());
    const RDGTexture next = resources->CreateTexture(LogicalTexture());
    graph.AddTransferPass("next_clear").NeverCull().ClearTexture(next, Color(0.f));
    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    EXPECT_NE(DescribeResource(resources, next).physicalStableId, id);
    ASSERT_TRUE(resources->TrimPool(true));
    CompleteGraphicsAndTransfer();
    EXPECT_FALSE(destroyed.contains(id));
    EXPECT_GT(resources->GetPoolStats().retiringBytes, 0u);
    ASSERT_TRUE(GDynamicRHI->WaitForSubmission(
        RHICommandContextType::eAsyncCompute,
        compute.completion.Get(RHICommandContextType::eAsyncCompute)));
    device->CollectCompletedResources();
    EXPECT_TRUE(destroyed.contains(id));
    EXPECT_EQ(resources->GetPoolStats().retiringBytes, 0u);
}

TEST_P(AsyncComputeLifetimeTest, FailedComputeWaitPreventsFrameSlotReuseAndOwnerRelease)
{
    RHIBuffer* buffer = Buffer();
    const uint64_t id = buffer->GetStableId();
    SubmitCompute(buffer);
    device->DestroyBuffer(buffer);
    CompleteGraphicsAndTransfer();
    EXPECT_FALSE(destroyed.contains(id));
    const uint32_t begins   = rhi->frameBegins;
    rhi->failSubmissionWait = true;
    device->NextFrame();
    device->NextFrame();
    device->FlushRHIThread();
    EXPECT_EQ(rhi->frameBegins, begins + 1);
    EXPECT_FALSE(destroyed.contains(id));
    ASSERT_FALSE(rhi->submissionWaits.empty());
    EXPECT_EQ(rhi->submissionWaits.back().first, RHICommandContextType::eAsyncCompute);
    const RenderFrameSlot slot = GRenderFrameState.GetFrameSlot();
    rhi->failSubmissionWait    = false;
    device->NextFrame();
    device->FlushRHIThread();
    EXPECT_EQ(GRenderFrameState.GetFrameSlot(), slot);
    EXPECT_EQ(rhi->frameBegins, begins + 2);
    EXPECT_TRUE(destroyed.contains(id));
    EXPECT_EQ(rhi->deviceIdleWaits, 0u);
}

TEST_P(AsyncComputeLifetimeTest, StagingHonorsComputeUsersWithoutWaitingForUnrelatedWork)
{
    StagingBufferManager staging(16, 16);
    StagingAllocation allocation, reused;
    ASSERT_EQ(staging.Allocate(16, 4, &allocation), StagingFlushAction::eNone);
    const RHIBatchResult compute = SubmitCompute(allocation.pBuffer);
    staging.Release(allocation, compute.completion);
    CompleteGraphicsAndTransfer();
    EXPECT_EQ(staging.Allocate(16, 4, &reused), StagingFlushAction::eFlush);
    ASSERT_TRUE(staging.WaitForSubmittedAllocations());
    EXPECT_EQ(staging.Allocate(16, 4, &reused), StagingFlushAction::eNone);
    EXPECT_EQ(reused.pBuffer, allocation.pBuffer);
    staging.Release(reused, {});
    staging.Destroy();

    StagingBufferManager uploadStaging(64, 64);
    StagingUploadQueue uploads(device, &uploadStaging);
    RHIBuffer* destination         = Buffer();
    const RHIBatchResult unrelated = SubmitCompute(nullptr);
    const SmallVector<uint8_t, 64> bytes(64);
    uploads.EnqueueBuffer(destination, 0, bytes.size(), bytes.data());
    ASSERT_TRUE(uploads.Flush());
    CompleteGraphicsAndTransfer();
    uploads.ReclaimResources();
    EXPECT_LT(GDynamicRHI->GetLastCompletedSerial(RHICommandContextType::eAsyncCompute),
              unrelated.completion.Get(RHICommandContextType::eAsyncCompute));
    ASSERT_EQ(uploadStaging.Allocate(64, 4, &reused), StagingFlushAction::eNone);
    uploadStaging.Release(reused, {});
    uploads.Destroy();
    uploadStaging.Destroy();
    device->DestroyBuffer(destination);
}

TEST_P(AsyncComputeLifetimeTest, ColdAllocationUsesDeviceServiceAndPreservesDescriptorAndOwnership)
{
    TestBuffer* source = Buffer(128);
    RenderGraph graph("service_materialization");
    RDGResourceManager* resources = graph.GetResourceManager();
    ASSERT_TRUE(graph.Begin());
    RDGBufferDesc bufferDesc = LogicalBuffer();
    bufferDesc.name          = "service_buffer";
    bufferDesc.size          = 128;
    bufferDesc.usageFlags.SetFlag(RHIBufferUsageFlagBits::eIndirectBuffer);
    RDGTextureDesc textureDesc          = LogicalTexture(3);
    textureDesc.name                    = "service_texture";
    textureDesc.texFormat.dimension     = TextureDimension::e3D;
    textureDesc.texFormat.width         = 16;
    textureDesc.texFormat.depth         = 4;
    textureDesc.texFormat.mutableFormat = true;
    const RDGBuffer buffer              = resources->CreateBuffer(bufferDesc);
    const RDGTexture texture            = resources->CreateTexture(textureDesc);
    graph.AddTransferPass("copy").CopyBuffer(resources->ImportHostWrittenBuffer(source), buffer,
                                             {0, 0, 128});
    graph.AddTransferPass("clear").ClearTexture(texture, Color(0.f));
    RDGExtractedBuffer outputBuffer   = resources->QueueBufferExtraction(buffer);
    RDGExtractedTexture outputTexture = resources->QueueTextureExtraction(texture);
    ASSERT_TRUE(graph.End());
    RDGExecutor executor(device);
    bool prepared = false;
    {
        struct RestoreRHI
        {
            DynamicRHI* facade{GDynamicRHI};
            ~RestoreRHI()
            {
                GDynamicRHI = facade;
            }
        } restore;
        GDynamicRHI = nullptr; // No global backend fallback during cold materialization.
        prepared    = executor.Prepare(&graph);
    }
    ASSERT_TRUE(prepared);
    EXPECT_FALSE(outputBuffer);
    EXPECT_FALSE(outputTexture);
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    ASSERT_TRUE(outputBuffer);
    ASSERT_TRUE(outputTexture);
    EXPECT_EQ(rhi->lastBufferInfo.size, bufferDesc.size);
    EXPECT_EQ(rhi->lastBufferInfo.tag, bufferDesc.name);
    EXPECT_EQ(rhi->lastBufferInfo.allocateType, RHIBufferAllocateType::eGPU);
    BitField<RHIBufferUsageFlagBits> bufferUsage = bufferDesc.usageFlags;
    bufferUsage.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);
    EXPECT_EQ(rhi->lastBufferInfo.usageFlags, bufferUsage);
    const RHITextureCreateInfo& info = outputTexture.Get()->GetBaseInfo();
    EXPECT_EQ(info.tag, textureDesc.name);
    EXPECT_EQ(info.type, RHITextureType::e3D);
    EXPECT_EQ(info.format, textureDesc.texFormat.format);
    EXPECT_EQ(info.width, 16u);
    EXPECT_EQ(info.height, 8u);
    EXPECT_EQ(info.depth, 4u);
    EXPECT_EQ(info.arrayLayers, 1u);
    EXPECT_EQ(info.mipmaps, 3u);
    EXPECT_EQ(info.samples, SampleCount::e1);
    EXPECT_TRUE(info.mutableFormat);
    BitField<RHITextureUsageFlagBits> textureUsage = textureDesc.usageFlags;
    textureUsage.SetFlag(RHITextureUsageFlagBits::eTransferDst);
    EXPECT_EQ(info.usageFlags, textureUsage);
    EXPECT_EQ(rhi->lastBufferCreationThread == std::this_thread::get_id(),
              GetParam() == RHIExecutionMode::eInline);
    EXPECT_EQ(rhi->lastTextureCreationThread, rhi->lastBufferCreationThread);
    const uint64_t bufferId  = outputBuffer.Get()->GetStableId();
    const uint64_t textureId = outputTexture.Get()->GetStableId();
    ASSERT_TRUE(graph.Reset());
    CompleteGraphicsAndTransfer();
    EXPECT_EQ(outputBuffer.Get()->GetRefCount(), 1u);
    EXPECT_EQ(outputTexture.Get()->GetRefCount(), 1u);
    const RHIBatchResult compute = SubmitCompute(outputBuffer.Get());
    outputBuffer.Reset();
    outputTexture.Reset();
    CompleteGraphicsAndTransfer();
    EXPECT_FALSE(destroyed.contains(bufferId));
    EXPECT_FALSE(destroyed.contains(textureId));
    ASSERT_TRUE(GDynamicRHI->WaitForSubmission(
        RHICommandContextType::eAsyncCompute,
        compute.completion.Get(RHICommandContextType::eAsyncCompute)));
    device->CollectCompletedResources();
    EXPECT_TRUE(destroyed.contains(bufferId));
    EXPECT_TRUE(destroyed.contains(textureId));
    device->DestroyBuffer(source);
}

TEST_P(AsyncComputeLifetimeTest, AllocationFailureRollsBackWithoutPublishingStateOrExtractions)
{
    TestBuffer* source = Buffer();
    for (bool failBuffer : {false, true})
    {
        RenderGraph graph("allocation_failure");
        RDGResourceManager* resources = graph.GetResourceManager();
        ASSERT_TRUE(graph.Begin());
        const RDGBuffer first = resources->CreateBuffer(LogicalBuffer());
        const RDGBuffer input = resources->ImportHostWrittenBuffer(source);
        graph.AddTransferPass("first").CopyBuffer(input, first, {0, 0, 64});
        RDGExtractedBuffer firstOutput = resources->QueueBufferExtraction(first);
        const RDGBuffer second         = resources->CreateBuffer(LogicalBuffer());
        graph.AddTransferPass("second").CopyBuffer(input, second, {0, 0, 64});
        RDGExtractedBuffer secondOutput = resources->QueueBufferExtraction(second);
        const RDGTexture texture        = resources->CreateTexture(LogicalTexture());
        graph.AddTransferPass("texture").ClearTexture(texture, Color(0.f));
        RDGExtractedTexture textureOutput = resources->QueueTextureExtraction(texture);
        ASSERT_TRUE(graph.End());
        rhi->failBufferCreationAt     = failBuffer ? rhi->bufferCreations + 2 : 0;
        rhi->failTextureCreationAt    = failBuffer ? 0 : rhi->textureCreations + 1;
        const RHICompletionSet before = device->GetSubmittedCompletion();
        EXPECT_FALSE(device->ExecuteRenderGraph(graph));
        EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eAllocation);
        const RHICompletionSet after = device->GetSubmittedCompletion();
        EXPECT_TRUE(std::equal(after.serials.begin(), after.serials.end(), before.serials.begin(),
                               before.serials.end()));
        EXPECT_FALSE(firstOutput);
        EXPECT_FALSE(secondOutput);
        EXPECT_FALSE(textureOutput);
        EXPECT_EQ(resources->GetPoolStats().assignedCount, 0u);
        EXPECT_FALSE(RDGSubmissionTestAccess::HasHistory(*device, rhi->lastBufferId));
        device->CollectCompletedResources();
        EXPECT_TRUE(destroyed.contains(rhi->lastBufferId));
        rhi->failBufferCreationAt  = 0;
        rhi->failTextureCreationAt = 0;
    }
    device->DestroyBuffer(source);
}

TEST_P(AsyncComputeLifetimeTest, PooledPreparationAndRecordingOnlyReadPublishedProgress)
{
    RDGExecutor recorder(device);
    RHICommandListPtr commands(
        RHICommandList::Create(GDynamicRHI->GetCommandContext(RHICommandContextType::eGraphics)));
    RenderGraph graph("snapshot_only_recording");
    RDGResourceManager* resources = graph.GetResourceManager();
    ASSERT_TRUE(graph.Begin());
    const RDGTexture initial = resources->CreateTexture(LogicalTexture());
    graph.AddTransferPass("warm").NeverCull().ClearTexture(initial, Color(0.f));
    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(recorder.Execute(&graph, commands.get()));
    const uint64_t id = DescribeResource(resources, initial).physicalStableId;
    commands->Reset();
    ASSERT_TRUE(graph.Reset());
    CompleteGraphicsAndTransfer();
    ASSERT_TRUE(graph.Begin());
    const RDGTexture reused = resources->CreateTexture(LogicalTexture());
    graph.AddTransferPass("reused").NeverCull().ClearTexture(reused, Color(0.f));
    ASSERT_TRUE(graph.End());
    device->FlushRHIThread();
    const uint64_t queries     = rhi->progressQueries;
    const uint32_t submissions = rhi->submissionAttempts;
    const uint32_t allocations = rhi->textureCreations;
    ASSERT_TRUE(recorder.Prepare(&graph));
    ASSERT_TRUE(recorder.Execute(&graph, commands.get()));
    EXPECT_EQ(DescribeResource(resources, reused).physicalStableId, id);
    EXPECT_EQ(resources->GetPoolStats().hits, 1u);
    EXPECT_EQ(rhi->progressQueries, queries);
    EXPECT_EQ(rhi->submissionAttempts, submissions);
    EXPECT_EQ(rhi->textureCreations, allocations);
    commands->Reset();
}

class SingleFrameComputeLifetimeTest : public AsyncComputeLifetimeTest
{
protected:
    void SetUp() override
    {
        InitializeDevice(nullptr, 1, GetParam(), true);
    }
};

TEST_P(SingleFrameComputeLifetimeTest, ComputeCompletionIsRequiredBeforeReusingTheOnlySlot)
{
    RHIBuffer* buffer = Buffer();
    const uint64_t id = buffer->GetStableId();
    SubmitCompute(buffer);
    device->DestroyBuffer(buffer);
    CompleteGraphicsAndTransfer();
    rhi->failSubmissionWait = true;
    device->NextFrame();
    device->FlushRHIThread();
    EXPECT_EQ(rhi->frameBegins, 1u);
    EXPECT_FALSE(destroyed.contains(id));
    rhi->failSubmissionWait = false;
    device->NextFrame();
    device->FlushRHIThread();
    EXPECT_EQ(rhi->frameBegins, 2u);
    EXPECT_TRUE(destroyed.contains(id));
    EXPECT_EQ(rhi->deviceIdleWaits, 0u);
}

INSTANTIATE_TEST_SUITE_P(ExecutionModes,
                         SingleFrameComputeLifetimeTest,
                         testing::Values(RHIExecutionMode::eInline, RHIExecutionMode::eThreaded));

TEST_F(RHIExecutorTest, PendingComputeTicketBecomesAComputeCompletionRequirement)
{
    RHIBufferCreateInfo info;
    info.size         = 64;
    RHIBuffer* buffer = executor->CreateBuffer(info);
    const uint64_t id = buffer->GetStableId();
    RHICommandListPtr commands(
        RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eAsyncCompute)));
    commands->RetainResource(buffer);
    uint32_t destructions = 0;
    RecordArenaData(*commands, 37, destructions);
    ArmGate();
    RHIRetirementRequirement retirement;
    retirement.pending.push_back(executor->SubmitFrame(*commands, nullptr));
    ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    buffer->ReleaseReference();
    EXPECT_FALSE(retirement.IsCompleteAt(RHICompletionSet{{1000, 0, 1000}}));
    EXPECT_FALSE(destroyed.contains(id));
    gate.Open();
    const RHIBatchResult result = retirement.pending[0].Wait();
    executor->FlushRHIThread();
    EXPECT_EQ(result.completion.Get(RHICommandContextType::eGraphics), 0u);
    EXPECT_GT(result.completion.Get(RHICommandContextType::eAsyncCompute), 0u);
    EXPECT_FALSE(retirement.IsCompleteAt(RHICompletionSet{{1000, 0, 1000}}));
    EXPECT_EQ(destructions, 0u);
    EXPECT_FALSE(destroyed.contains(id));
    ASSERT_TRUE(
        executor->WaitForSubmission(RHICommandContextType::eAsyncCompute,
                                    result.completion.Get(RHICommandContextType::eAsyncCompute)));
    EXPECT_TRUE(retirement.IsCompleteAt(executor->GetCompletedCompletion()));
    EXPECT_EQ(destructions, 3u);
    EXPECT_TRUE(destroyed.contains(id));
}

INSTANTIATE_TEST_SUITE_P(ExecutionModes,
                         AsyncComputeLifetimeTest,
                         testing::Values(RHIExecutionMode::eInline, RHIExecutionMode::eThreaded));
} // namespace
