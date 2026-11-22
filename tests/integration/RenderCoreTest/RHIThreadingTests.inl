#include "Utils/Errors.h"
#include <array>
#include <chrono>

#if defined(ZEN_WIN32)
#    include <Windows.h>
#endif

namespace
{
class ThreadedRenderCoreTest : public RenderCoreTest
{
protected:
    void SetUp() override
    {
        InitializeDevice(nullptr, 3, RHIExecutionMode::eThreaded);
        device->FlushRHIThread();
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
} // namespace


TEST(RHIQueueCapabilitiesTest, RenderDeviceResolvesStartupPolicyBeforeInitialization)
{
    struct Case
    {
        AsyncComputeMode     mode;
        RHIQueueCapabilities capabilities;
        AsyncComputeStatus   status;
    };
    const Case cases[] = {{AsyncComputeMode::eDisabled, {true, true, {0, 1, 2}}, AsyncComputeStatus::eDisabled},
                          {AsyncComputeMode::eAuto, {false, true, {0, 1, 2}}, AsyncComputeStatus::eComputeUnavailable},
                          {AsyncComputeMode::eAuto, {true, true, {0, 0, 0}}, AsyncComputeStatus::eSharedGraphicsQueue},
                          {AsyncComputeMode::eAuto, {true, false, {0, 1, 1}}, AsyncComputeStatus::eDependenciesUnavailable},
                          {AsyncComputeMode::eAuto, {true, true, {0, 1, 1}}, AsyncComputeStatus::eAvailable}};
    for (RHIExecutionMode cpuMode : {RHIExecutionMode::eInline, RHIExecutionMode::eThreaded})
    {
        for (const Case& test : cases)
        {
            TestRHI* backend                     = ZEN_NEW() TestRHI();
            backend->submissionQueueCapabilities = test.capabilities;
            GDynamicRHI                          = backend;
            RenderDevice device(RHIAPIType::eVulkan, 2, cpuMode, test.mode);
            EXPECT_EQ(device.GetAsyncComputeStatus(), test.status);
            const std::array<uint32_t, RHICompletionSet::kQueueCount>& queueIds = device.GetQueueCapabilities().queueIds;
            EXPECT_TRUE(std::equal(queueIds.begin(), queueIds.end(), test.capabilities.queueIds.begin(),
                                   test.capabilities.queueIds.end()));
            EXPECT_EQ(backend->queueCapabilityQueries, 1u);
            device.Init(nullptr);
            EXPECT_EQ(backend->submissionAttempts, 0u);
            EXPECT_EQ(device.GetAsyncComputeStatus(), test.status);
            device.Destroy();
        }
    }
}






#if defined(ZEN_WIN32)







TEST_F(ThreadedRenderCoreTest, WindowsFrameTicketWaitServicesSentMessages)
{
    RHIMessageWindow window;
    ASSERT_NE(window.handle, nullptr);
    RenderGraph* graph = device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph->Begin());
    RDGTexture texture = graph->GetResourceManager()->CreateTexture(LogicalTexture());
    graph->AddTransferPass("window-message").NeverCull().ClearTexture(texture, Color(0.f));
    ASSERT_TRUE(graph->End());
    device->FlushRHIThread();
    rhi->beforeSubmission = std::bind_front(&RHIMessageWindow::Send, &window);
    const bool queued     = device->ExecuteRenderGraph(&viewport) == RenderFrameResult::eSubmitted;
    const bool completed  = device->PollFrameSubmissions(true);
    GetRHIThread().Flush();
    rhi->beforeSubmission = {};
    EXPECT_TRUE(queued);
    EXPECT_TRUE(completed);
    // One message per native flush; the frame and its presentation copy share one flush.
    EXPECT_EQ(window.deliveredCount, 1u) << "Win32 error " << window.lastError;
    EXPECT_EQ(viewport.presents, 1u);
}
#endif




TEST_F(ThreadedRenderCoreTest, NextFrameCanBuildWhileRhiIsBlockedAndExtractionWaitsForConfirmation)
{
    RenderGraph* graph = device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph->Begin());
    RDGTexture texture = graph->GetResourceManager()->CreateTexture(LogicalTexture());
    graph->AddTransferPass("clear").ClearTexture(texture, Color(0.f));
    RDGExtractedTexture output = graph->GetResourceManager()->QueueTextureExtraction(texture);
    ASSERT_TRUE(graph->End());
    ArmGate();
    ASSERT_EQ(device->ExecuteRenderGraph(&viewport), RenderFrameResult::eSubmitted);
    ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_FALSE(output);
    device->NextFrame();
    EXPECT_EQ(ToValue(GRenderFrameState.GetFrameNumber()), 1u);
    // Resetting the old graph must not invalidate its queued commands or extraction.
    ASSERT_TRUE(graph->Begin());
    EXPECT_FALSE(output);
    EXPECT_FALSE(gate.releasedInTime);
    gate.Open();
    device->FlushRHIThread();
    ASSERT_TRUE(output);
    EXPECT_TRUE(gate.releasedInTime);
    EXPECT_NE(gate.executionThread, std::this_thread::get_id());
    EXPECT_EQ(device->GetRHIThreadMetrics().completedBatches, 1u);
    EXPECT_EQ(viewport.presents, 1u);
    output.Reset();
    device->WaitForPreviousFrames();
}

TEST_F(ThreadedRenderCoreTest, LateFailureDoesNotPublishExtractionOrSubmitNextFrame)
{
    RenderGraph* graph = device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph->Begin());
    RDGTexture texture = graph->GetResourceManager()->CreateTexture(LogicalTexture());
    graph->AddTransferPass("clear").ClearTexture(texture, Color(0.f));
    RDGExtractedTexture output = graph->GetResourceManager()->QueueTextureExtraction(texture);
    ASSERT_TRUE(graph->End());
    rhi->failSubmissionAt = 1;
    ArmGate();
    ASSERT_EQ(device->ExecuteRenderGraph(&viewport), RenderFrameResult::eSubmitted);
    ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    gate.Open();
    device->FlushRHIThread();
    EXPECT_TRUE(device->AreSubmissionsBlocked());
    EXPECT_FALSE(output);
    EXPECT_NE(device->ExecuteRenderGraph(&viewport), RenderFrameResult::eSubmitted);
    EXPECT_EQ(rhi->submissionAttempts, 1u);
}

TEST_F(ThreadedRenderCoreTest, NextFrameRecordsAndSubmitsBeforePriorSubmissionCompletes)
{
    CreateTestShaderProgram(device, "intent");
    RenderGraph*             graph = device->GetCurrentFrameRDG();
    const RDGComputePassDesc pass  = IntentPass();
    // Warm the pipeline cache and producer command list before measuring overlap.
    ASSERT_TRUE(graph->Begin());
    graph->AddComputePass(pass).RecordPassCommands([](RDGPassCmdEncoder& encoder) { encoder.Dispatch(1, 1, 1); });
    ASSERT_TRUE(graph->End());
    ASSERT_TRUE(device->ExecuteRenderGraph(*graph));

    const uint64_t completedBatches = device->GetRHIThreadMetrics().completedBatches;
    ASSERT_TRUE(graph->Begin());
    graph->AddComputePass(pass).RecordPassCommands([](RDGPassCmdEncoder& encoder) { encoder.Dispatch(1, 1, 1); });
    ASSERT_TRUE(graph->End());
    ArmGate();
    ASSERT_EQ(device->ExecuteRenderGraph(&viewport), RenderFrameResult::eSubmitted);
    ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    device->NextFrame();
    const uint64_t  frameWait = device->GetFrameSubmissionWaitNS();
    const uint64_t  groupWait = device->GetRHIThreadMetrics().submitGroupsWaitNS;
    std::thread::id recordingThread;
    ASSERT_TRUE(graph->Begin());
    graph->AddComputePass(pass).RecordPassCommands([&recordingThread](RDGPassCmdEncoder& encoder) {
        recordingThread = std::this_thread::get_id();
        encoder.Dispatch(1, 1, 1);
    });
    ASSERT_TRUE(graph->End());
    ASSERT_EQ(device->ExecuteRenderGraph(&viewport), RenderFrameResult::eSubmitted);
    EXPECT_EQ(RDGSubmissionTestAccess::PendingFrameCount(*device), 2u);
    EXPECT_EQ(device->GetFrameSubmissionWaitNS(), frameWait);
    EXPECT_EQ(device->GetRHIThreadMetrics().submitGroupsWaitNS, groupWait);
    gate.Open();
    device->FlushRHIThread();
    EXPECT_TRUE(gate.releasedInTime);
    EXPECT_EQ(recordingThread, std::this_thread::get_id());
    EXPECT_NE(recordingThread, gate.executionThread);
    EXPECT_EQ(device->GetRHIThreadMetrics().completedBatches, completedBatches + 2u);
    EXPECT_EQ(device->GetRHIThreadMetrics().peakPendingBatches, 2u);
}


TEST_F(ThreadedRenderCoreTest, FrameSlotReuseWaitsForItsGpuSerial)
{
    for (uint32_t i = 0; i < 5; ++i)
    {
        RenderGraph* graph = device->GetCurrentFrameRDG();
        ASSERT_TRUE(graph->Begin());
        RDGTexture texture = graph->GetResourceManager()->CreateTexture(LogicalTexture());
        graph->AddTransferPass("clear").NeverCull().ClearTexture(texture, Color(0.f));
        ASSERT_TRUE(graph->End());
        ASSERT_EQ(device->ExecuteRenderGraph(&viewport), RenderFrameResult::eSubmitted);
        device->NextFrame();
    }
    device->FlushRHIThread();
    EXPECT_EQ(device->GetRHIThreadMetrics().completedBatches, 5u);
    EXPECT_FALSE(rhi->submissionWaits.empty());
    EXPECT_EQ(ToValue(GRenderFrameState.GetFrameNumber()), 5u);
    EXPECT_EQ(ToValue(GRHIFrameState.GetFrameNumber()), 5u);
    device->WaitForPreviousFrames();
}

TEST_F(ThreadedRenderCoreTest, DelayedCompletionUsesOriginatingRenderFrameWithDifferentRhiTimeline)
{
    GetRHIThread().Invoke(&RHIFrameState::Init, &GRHIFrameState, 2);
    GetRHIThread().Invoke(&RHIFrameState::Advance, &GRHIFrameState);
    RenderGraph* graph = device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph->Begin());
    RDGTexture texture = graph->GetResourceManager()->CreateTexture(LogicalTexture());
    graph->AddTransferPass("clear").NeverCull().ClearTexture(texture, Color(0.f));
    ASSERT_TRUE(graph->End());
    ArmGate();
    ASSERT_EQ(device->ExecuteRenderGraph(&viewport), RenderFrameResult::eSubmitted);
    ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(ToValue(GRenderFrameState.GetFrameNumber()), 0u);
    EXPECT_EQ(ToValue(gate.executionFrameState.GetFrameNumber()), 1u);
    EXPECT_EQ(ToIndex(gate.executionFrameState.GetFrameSlot()), 1u);
    device->NextFrame();
    EXPECT_EQ(ToIndex(GRenderFrameState.GetFrameSlot()), 1u);
    gate.Open();
    device->FlushRHIThread();
    const uint64_t firstSerial = GDynamicRHI->GetLastSubmittedSerial(RHICommandContextType::eGraphics);
    ASSERT_GT(firstSerial, 0u);
    // Return to RenderCore slot 0 while the independent RHI timeline uses two slots.
    for (uint32_t i = 1; i < 3; ++i)
    {
        ASSERT_TRUE(graph->Begin());
        texture = graph->GetResourceManager()->CreateTexture(LogicalTexture());
        graph->AddTransferPass("clear").NeverCull().ClearTexture(texture, Color(0.f));
        ASSERT_TRUE(graph->End());
        ASSERT_EQ(device->ExecuteRenderGraph(&viewport), RenderFrameResult::eSubmitted);
        device->NextFrame();
    }
    device->FlushRHIThread();
    ASSERT_FALSE(rhi->submissionWaits.empty());
    EXPECT_EQ(rhi->submissionWaits[0].first, RHICommandContextType::eGraphics);
    EXPECT_EQ(rhi->submissionWaits[0].second, firstSerial);
    EXPECT_EQ(ToValue(GRenderFrameState.GetFrameNumber()), 3u);
    EXPECT_EQ(ToValue(GRHIFrameState.GetFrameNumber()), 4u);
    EXPECT_EQ(ToIndex(GRenderFrameState.GetFrameSlot()), 0u);
    EXPECT_EQ(ToIndex(GRHIFrameState.GetFrameSlot()), 0u);
}

TEST_F(ThreadedRenderCoreTest, PooledResourcesWaitForPendingCPUAndGPUWork)
{
    CreateTestShaderProgram(device, "intent");
    RenderGraph*        graph     = device->GetCurrentFrameRDG();
    RDGResourceManager* resources = graph->GetResourceManager();
    uint64_t            bufferId  = 0;
    uint64_t            textureId = 0;
    ResourceRetirement  retirement;
    for (uint32_t phase = 0; phase < 3; ++phase)
    {
        ASSERT_TRUE(graph->Begin());
        const RDGBuffer    buffer  = resources->CreateBuffer(LogicalBuffer());
        const RDGTexture   texture = resources->CreateTexture(LogicalTexture());
        RDGComputePassDesc pass    = IntentPass();
        pass.BindStorageBuffer("write_buffer", buffer, RDGContentGuarantee::eFullWrite);
        pass.BindStorageImage("write_image", texture, RDGContentGuarantee::eFullWrite);
        graph->AddComputePass(pass).RecordPassCommands([](RDGPassCmdEncoder& encoder) { encoder.Dispatch(1, 1, 1); });
        ASSERT_TRUE(graph->End());
        if (phase == 0)
        {
            ArmGate();
            ASSERT_EQ(device->ExecuteRenderGraph(&viewport), RenderFrameResult::eSubmitted);
            ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
            bufferId   = DescribeResource(resources, buffer).physicalStableId;
            textureId  = DescribeResource(resources, texture).physicalStableId;
            retirement = device->CaptureResourceRetirement();
            EXPECT_FALSE(retirement.pending.empty());
            EXPECT_EQ(retirement.requiredSerials.Get(RHICommandContextType::eGraphics), 0u);
            EXPECT_FALSE(device->IsResourceRetired(retirement));
            ASSERT_TRUE(graph->Reset());
            EXPECT_GT(resources->GetPoolStats().inFlightBytes, 0u);
            EXPECT_FALSE(destroyed.contains(bufferId));
            EXPECT_FALSE(destroyed.contains(textureId));
            // Cold allocation runs on the same RHI worker; open the CPU gate before
            // preparing the next graph, while leaving GPU completion at zero.
            gate.Open();
            device->FlushRHIThread();
            EXPECT_FALSE(device->IsResourceRetired(retirement));
        }
        else
        {
            ASSERT_TRUE(device->ExecuteRenderGraph(*graph));
            if (phase == 1)
            {
                EXPECT_NE(DescribeResource(resources, buffer).physicalStableId, bufferId);
                EXPECT_NE(DescribeResource(resources, texture).physicalStableId, textureId);
                ASSERT_TRUE(graph->Reset());
                GDynamicRHI->WaitDeviceIdle();
                device->FlushRHIThread();
                EXPECT_TRUE(device->IsResourceRetired(retirement));
            }
            else
            {
                EXPECT_EQ(resources->GetPoolStats().hits, 2u);
                EXPECT_EQ(resources->GetPoolStats().misses, 4u);
            }
        }
    }
    EXPECT_TRUE(gate.releasedInTime);
}

TEST_F(ThreadedRenderCoreTest, IdleCollectionRefreshesProgressAndRetiresBothKindsOfOwners)
{
    RHICommandListExecutor* executor = static_cast<RHICommandListExecutor*>(GDynamicRHI);
    RHITextureCreateInfo    info{};
    RHITexture*             batchOwned  = executor->CreateTexture(info);
    RHITexture*             deviceOwned = executor->CreateTexture(info);
    const uint64_t          batchId     = batchOwned->GetStableId();
    const uint64_t          deviceId    = deviceOwned->GetStableId();
    RHICommandListPtr       commands(RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));
    commands->ClearTexture(batchOwned, Color(0.f), RHITextureSubResourceRange());
    commands->ClearTexture(deviceOwned, Color(0.f), RHITextureSubResourceRange());
    const RHIBatchResult result = executor->SubmitFrame(*commands, nullptr).Wait();
    ASSERT_GT(result.requiredSerials.Get(RHICommandContextType::eGraphics), 0u);
    batchOwned->ReleaseReference();
    device->DeferReleaseResource(deviceOwned);
    device->CollectCompletedResources();
    GetRHIThread().Flush();
    EXPECT_EQ(executor->QueryLastCompletedSerial(RHICommandContextType::eGraphics), 0u);
    EXPECT_FALSE(destroyed.contains(batchId));
    EXPECT_FALSE(destroyed.contains(deviceId));

    // Complete after the last publication, without starting or submitting another frame.
    GetRHIThread().Invoke([this] { rhi->completed = rhi->submitted; });
    device->CollectCompletedResources();
    GetRHIThread().Flush(); // Only fence the worker; do not refresh through executor Flush.
    EXPECT_EQ(executor->QueryLastCompletedSerial(RHICommandContextType::eGraphics),
              result.requiredSerials.Get(RHICommandContextType::eGraphics));
    EXPECT_TRUE(destroyed.contains(batchId));
    device->CollectCompletedResources(); // Consume the published progress on RenderCore.
    GetRHIThread().Flush();
    EXPECT_TRUE(destroyed.contains(deviceId));
    EXPECT_TRUE(rhi->submissionWaits.empty());
    EXPECT_EQ(rhi->deviceIdleWaits, 0u);
}

TEST_F(ThreadedRenderCoreTest, FinalRhiReleaseRemovesBufferAndTextureHistory)
{
    RHITexture*    texture   = Texture();
    RHIBuffer*     buffer    = Buffer();
    const uint64_t textureId = texture->GetStableId();
    const uint64_t bufferId  = buffer->GetStableId();
    RenderGraph*   graph     = device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph->Begin());
    graph->GetResourceManager()->ImportHostWrittenBuffer(buffer);
    graph->AddTransferPass("retire-history")
        .NeverCull()
        .ClearTexture(texture, Color(0.f))
        .CopyBuffer(buffer, buffer, {0, 32, 4});
    ASSERT_TRUE(graph->End());
    ArmGate();
    ASSERT_EQ(device->ExecuteRenderGraph(&viewport), RenderFrameResult::eSubmitted);
    ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    device->NextFrame();
    // Retire RenderCore owners before the previous batch publishes its serial.
    ASSERT_TRUE(graph->Begin());
    device->DestroyTexture(texture);
    device->DestroyBuffer(buffer);
    gate.Open();
    GetRHIThread().Flush();
    device->CollectCompletedResources();
    GetRHIThread().Flush();
    EXPECT_FALSE(destroyed.contains(textureId));
    EXPECT_FALSE(destroyed.contains(bufferId));
    EXPECT_TRUE(RDGSubmissionTestAccess::HasHistory(*device, textureId));
    EXPECT_TRUE(RDGSubmissionTestAccess::HasHistory(*device, bufferId));

    GetRHIThread().Invoke([this] { rhi->completed = rhi->submitted; });
    device->CollectCompletedResources();
    GetRHIThread().Flush();
    device->CollectCompletedResources();
    GetRHIThread().Flush();
    EXPECT_TRUE(destroyed.contains(textureId));
    EXPECT_TRUE(destroyed.contains(bufferId));
    EXPECT_FALSE(RDGSubmissionTestAccess::HasHistory(*device, textureId));
    EXPECT_FALSE(RDGSubmissionTestAccess::HasHistory(*device, bufferId));
    EXPECT_TRUE(rhi->submissionWaits.empty());
    EXPECT_EQ(rhi->deviceIdleWaits, 0u);
}

TEST_F(ThreadedRenderCoreTest, RetirementDuringRecordingCannotReappearFromPendingState)
{
    CreateTestShaderProgram(device, "intent");
    RHITexture*    texture = Texture();
    const uint64_t id      = texture->GetStableId();
    RDGSubmissionTestAccess::Tracker(*device).UpdateTextureState(
        texture, RHIAccessMode::eReadWrite, RHITextureUsage::eTransferDst,
        RHIFlags<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eTransfer));
    RenderGraph* graph = device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph->Begin());
    graph->AddComputePass(IntentPass()).RecordPassCommands([this, texture, id](RDGPassCmdEncoder& encoder) {
        texture->ReleaseReference();
        device->CollectCompletedResources();
        EXPECT_TRUE(RDGSubmissionTestAccess::HasHistory(*device, id));
        encoder.Dispatch(1, 1, 1);
    });
    ASSERT_TRUE(graph->End());
    ArmGate();
    ASSERT_EQ(device->ExecuteRenderGraph(&viewport), RenderFrameResult::eSubmitted);
    ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_EQ(RDGSubmissionTestAccess::PendingFrameCount(*device), 1u);
    EXPECT_TRUE(destroyed.contains(id));
    EXPECT_FALSE(RDGSubmissionTestAccess::HasHistory(*device, id));
    gate.Open();
    device->FlushRHIThread();
    EXPECT_EQ(RDGSubmissionTestAccess::PendingFrameCount(*device), 0u);
    EXPECT_FALSE(RDGSubmissionTestAccess::HasHistory(*device, id));
}

TEST_F(RenderCoreTest, InlineFinalReleasePreservesLiveHistoryAndCleansDestroyedHistory)
{
    RHITexture*           texture = Texture();
    const uint64_t        id      = texture->GetStableId();
    ResourceStateTracker& tracker = RDGSubmissionTestAccess::Tracker(*device);
    tracker.UpdateTextureState(texture, RHIAccessMode::eReadWrite, RHITextureUsage::eTransferDst,
                               RHIFlags<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eTransfer));
    texture->AddReference();
    texture->ReleaseReference();
    const uint64_t liveRevision = tracker.GetRevision();
    device->PollFrameSubmissions();
    EXPECT_FALSE(destroyed.contains(id));
    EXPECT_TRUE(RDGSubmissionTestAccess::HasHistory(*device, id));
    EXPECT_EQ(tracker.GetRevision(), liveRevision);

    texture->ReleaseReference();
    device->PollFrameSubmissions();
    EXPECT_TRUE(destroyed.contains(id));
    EXPECT_FALSE(RDGSubmissionTestAccess::HasHistory(*device, id));
    EXPECT_GT(tracker.GetRevision(), liveRevision);
    const uint64_t retiredRevision = tracker.GetRevision();
    device->PollFrameSubmissions();
    EXPECT_EQ(tracker.GetRevision(), retiredRevision);
}

TEST_F(RenderCoreTest, RetirementDuringRollbackCannotReappearFromPrivateState)
{
    CreateTestShaderProgram(device, "intent");
    RHITexture*    texture = Texture();
    const uint64_t id      = texture->GetStableId();
    RDGSubmissionTestAccess::Tracker(*device).UpdateTextureState(
        texture, RHIAccessMode::eReadWrite, RHITextureUsage::eTransferDst,
        RHIFlags<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eTransfer));
    RenderGraph graph("retirement_rollback");
    ASSERT_TRUE(graph.Begin());
    graph.AddComputePass(IntentPass()).RecordPassCommands([this, texture](RDGPassCmdEncoder& encoder) {
        texture->ReleaseReference();
        device->PollFrameSubmissions();
        encoder.Fail(RDGErrorCode::eCallback, "cancel after unrelated retirement");
    });
    ASSERT_TRUE(graph.End());
    EXPECT_FALSE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eCallback);
    EXPECT_TRUE(destroyed.contains(id));
    EXPECT_FALSE(RDGSubmissionTestAccess::HasHistory(*device, id));
}




TEST_F(ThreadedRenderCoreTest, ProgressQueryFailureBlocksRetirementWithoutAPendingFrame)
{
    RHICommandListExecutor* executor = static_cast<RHICommandListExecutor*>(GDynamicRHI);
    RHITextureCreateInfo    info{};
    RHITexture*             texture = executor->CreateTexture(info);
    const uint64_t          id      = texture->GetStableId();
    device->DeferReleaseResource(texture);
    GetRHIThread().Invoke([this] { rhi->failProgressQuery = true; });
    executor->PollGPUProgress();
    GetRHIThread().Flush();
    EXPECT_TRUE(device->AreSubmissionsBlocked());
    // Restore queries for cleanup; the terminal block must still retain native owners.
    GetRHIThread().Invoke([this] { rhi->failProgressQuery = false; });
    device->CollectCompletedResources();
    GetRHIThread().Flush();
    EXPECT_FALSE(destroyed.contains(id));
    viewport.ReleaseForTeardown();
    device->Destroy();
    EXPECT_TRUE(destroyed.contains(id));
}

TEST_F(ThreadedRenderCoreTest, BackendFailureRetainsBatchAndDeferredOwnersUntilShutdown)
{
    RHICommandListExecutor* executor = static_cast<RHICommandListExecutor*>(GDynamicRHI);
    RHITextureCreateInfo    info{};
    RHITexture*             batchOwned = executor->CreateTexture(info);
    RHITexture*             deferred   = executor->CreateTexture(info);
    const uint64_t          batchId    = batchOwned->GetStableId();
    const uint64_t          deferredId = deferred->GetStableId();
    RHICommandListPtr       commands(RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));
    commands->ClearTexture(batchOwned, Color(0.f), RHITextureSubResourceRange());
    EXPECT_EQ(executor->SubmitFrame(*commands, nullptr).Wait().submission, RHISubmissionResult::eSuccess);
    batchOwned->ReleaseReference();
    device->DeferReleaseResource(deferred);
    GetRHIThread().Invoke([this] { rhi->submissionsBlocked = true; });
    executor->PollGPUProgress();
    GetRHIThread().Flush();
    EXPECT_TRUE(device->AreSubmissionsBlocked());
    // Later successful queries must not clear a published terminal failure.
    GetRHIThread().Invoke([this] {
        rhi->submissionsBlocked = false;
        rhi->completed          = rhi->submitted;
    });
    device->CollectCompletedResources();
    GetRHIThread().Flush();
    EXPECT_TRUE(device->AreSubmissionsBlocked());
    EXPECT_FALSE(destroyed.contains(batchId));
    EXPECT_FALSE(destroyed.contains(deferredId));
    commands.reset();
    viewport.ReleaseForTeardown();
    device->Destroy();
    EXPECT_TRUE(destroyed.contains(batchId));
    EXPECT_TRUE(destroyed.contains(deferredId));
}

TEST_F(RenderCoreTest, PresentationRestoresClearOnlyBackbufferOnGraphics)
{
    TestViewport viewport;
    viewport.color     = Texture();
    RenderGraph* graph = device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph->Begin());
    graph->AddTransferPass("clear_only").ClearTexture(viewport.color, Color(0.25f));
    ASSERT_TRUE(graph->End());
    ASSERT_EQ(device->ExecuteRenderGraph(&viewport), RenderFrameResult::eSubmitted);
    ASSERT_FALSE(rhi->graphics.textureTransitions.empty());
    const RHITextureTransition& last = rhi->graphics.textureTransitions.back();
    EXPECT_EQ(last.pTexture, viewport.color);
    EXPECT_EQ(last.oldUsage, RHITextureUsage::eTransferDst);
    EXPECT_EQ(last.newUsage, RHITextureUsage::eColorAttachment);
    EXPECT_EQ(last.newAccessMode, RHIAccessMode::eReadWrite);
    EXPECT_TRUE(rhi->transfer.textureTransitions.empty());
    device->DestroyTexture(viewport.color);
    viewport.ReleaseForTeardown();
    device->Destroy();
}

TEST_F(RenderCoreTest, MatchingPresentationStateAddsNoBarrier)
{
    TestViewport viewport;
    viewport.color     = Texture();
    RenderGraph* graph = device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph->Begin());
    RDGTextureImportState initial{};
    initial.usage      = RHITextureUsage::eColorAttachment;
    initial.accessMode = RHIAccessMode::eReadWrite;
    initial.stages     = RHIFlags<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eColorAttachmentOutput);
    graph->GetResourceManager()->ImportTexture(viewport.color, initial);
    ASSERT_TRUE(graph->End());
    ASSERT_EQ(device->ExecuteRenderGraph(&viewport), RenderFrameResult::eSubmitted);
    EXPECT_TRUE(rhi->graphics.textureTransitions.empty());
    EXPECT_TRUE(device->GetRDGMetrics().GetLastSnapshot().validated);
    EXPECT_EQ(device->GetRDGMetrics().GetLastSnapshot().issues[size_t(RDGMetricIssue::eMissingBarrier)], 0u);
    device->DestroyTexture(viewport.color);
    viewport.ReleaseForTeardown();
    device->Destroy();
}

TEST_F(RHIExecutorTest, PartialFailureKeepsResourcesUntilDeviceDrain)
{
    RHITextureCreateInfo info{};
    RHITexture*          texture = executor->CreateTexture(info);
    const uint64_t       id      = texture->GetStableId();
    RHICommandListPtr    commands(RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));
    commands->ClearTexture(texture, Color(0.f), RHITextureSubResourceRange());
    rhi->failSubmissionAt       = 1;
    rhi->submitBeforeFailure    = true;
    rhi->submissionFailure      = RHISubmissionResult::eFatal;
    RHISubmissionTicket  ticket = executor->SubmitFrame(*commands, nullptr);
    const RHIBatchResult result = ticket.Wait();
    EXPECT_EQ(result.submission, RHISubmissionResult::eFatal);
    EXPECT_GT(result.requiredSerials.Get(RHICommandContextType::eGraphics), 0u);
    ResourceRetirement retirement;
    retirement.pending.push_back(ticket);
    EXPECT_FALSE(retirement.Resolve());
    EXPECT_FALSE(retirement.pending.empty());
    EXPECT_FALSE(retirement.IsCompleteAt(RHICompletionSet{{UINT64_MAX, UINT64_MAX, UINT64_MAX}}));
    texture->ReleaseReference();
    executor->WaitDeviceIdle();
    EXPECT_FALSE(destroyed.contains(id));
    commands.reset();
    executor->Destroy();
    EXPECT_TRUE(destroyed.contains(id));
}
