// Included by RenderCoreTests.cpp to exercise the same fake backend as RDG tests.
#if defined(ZEN_WIN32)
#    include <Windows.h>
#endif

namespace
{
struct ArenaDataCommand : RHICommand
{
    ArenaDataCommand(const uint8_t* bytes, uint32_t size, uint8_t value, uint32_t& destructions) :
        bytes(bytes), size(size), value(value), destructions(destructions)
    {}

    ~ArenaDataCommand() override
    {
        ++destructions;
    }

    void Execute(RHICommandListBase&) override
    {
        for (uint32_t i = 0; i < size; ++i)
        {
            ASSERT_EQ(bytes[i], value);
        }
    }

    const uint8_t* bytes;
    uint32_t size;
    uint8_t value;
    uint32_t& destructions;
};

void RecordArenaData(RHICommandList& commands, uint8_t value, uint32_t& destructions)
{
    for (uint32_t size : {1024u, 70u * 1024u, 256u * 1024u})
    {
        uint8_t* bytes = commands.AllocateCmdData<uint8_t>(size);
        std::memset(bytes, value, size);
        commands.AllocateCmdTyped<ArenaDataCommand>(bytes, size, value, destructions);
    }
}

class RHISubmissionGate
{
public:
    RHISubmissionGate() : m_release(m_open.get_future().share())
    {
        entered = m_entered.get_future();
    }
    ~RHISubmissionGate()
    {
        Open();
    }

    void OnSubmission()
    {
        if (!m_used)
        {
            m_used              = true;
            executionThread     = std::this_thread::get_id();
            executionFrameState = GRHIFrameState;
            m_entered.set_value();
            releasedInTime =
                m_release.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
        }
    }

    void Open()
    {
        if (!m_opened.exchange(true))
        {
            m_open.set_value();
        }
    }

    std::future<void> entered;
    std::thread::id executionThread;
    RHIFrameState executionFrameState;
    std::atomic<bool> releasedInTime{false};

private:
    std::promise<void> m_entered;
    std::promise<void> m_open;
    std::shared_future<void> m_release;
    std::atomic<bool> m_opened{false};
    bool m_used{false};
};

class RHIExecutorTest : public testing::Test
{
protected:
    void SetUp() override
    {
        destroyed.clear();
        GRHIFrameState.Init(3);
        rhi         = ZEN_NEW() TestRHI();
        executor    = ZEN_NEW() RHICommandListExecutor(rhi, RHIExecutionMode::eThreaded);
        GDynamicRHI = executor;
    }

    void TearDown() override
    {
        gate.Open();
        executor->Destroy();
        ZEN_DELETE(executor);
        GDynamicRHI = nullptr;
    }

    void ArmGate()
    {
        executor->FlushRHIThread();
        rhi->beforeSubmission = std::bind_front(&RHISubmissionGate::OnSubmission, &gate);
    }

    TestRHI* rhi{nullptr};
    RHICommandListExecutor* executor{nullptr};
    RHISubmissionGate gate;
};

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
        RenderCoreTest::TearDown();
    }

    void ArmGate()
    {
        device->FlushRHIThread();
        rhi->beforeSubmission = std::bind_front(&RHISubmissionGate::OnSubmission, &gate);
    }

    TestViewport viewport;
    RHISubmissionGate gate;
};
} // namespace

TEST(RHICommandListTest, PoolAllocatorReusesOverflowBlocksAfterReset)
{
    PoolAllocator<LinearAllocator> allocator(256);
    std::array<void*, 3> blocks{};
    const std::array<size_t, 3> sizes{192, 384, 1536};
    for (size_t i = 0; i < sizes.size(); ++i)
    {
        blocks[i] = allocator.Alloc(sizes[i], 64);
        ASSERT_NE(blocks[i], nullptr);
        EXPECT_EQ(reinterpret_cast<uintptr_t>(blocks[i]) % 64, 0u);
    }
    const size_t blockCount  = allocator.NumAllocators();
    const size_t allocations = DefaultAllocator::GetTrackedAllocationEvents();
    for (uint32_t frame = 0; frame < 32; ++frame)
    {
        allocator.Reset();
        for (size_t i = 0; i < sizes.size(); ++i)
        {
            EXPECT_EQ(allocator.Alloc(sizes[i], 64), blocks[i]);
        }
        EXPECT_EQ(allocator.NumAllocators(), blockCount);
    }
    EXPECT_EQ(DefaultAllocator::GetTrackedAllocationEvents(), allocations);
}

TEST(RHICommandListTest, DetachedStorageReusesAllArenaBlocksWithoutAllocating)
{
    uint32_t destructions = 0;
    RHICommandList commands;
    RHICommandListPtr reusable;
    for (uint8_t frame = 0; frame < 2; ++frame)
    {
        RecordArenaData(commands, frame, destructions);
        reusable = commands.DetachCommands(std::move(reusable));
        reusable->Execute();
        reusable->Reset();
    }
    const size_t allocations = DefaultAllocator::GetTrackedAllocationEvents();
    for (uint8_t frame = 2; frame < 18; ++frame)
    {
        RecordArenaData(commands, frame, destructions);
        reusable = commands.DetachCommands(std::move(reusable));
        EXPECT_EQ(commands.GetCommandCount(), 0u);
        EXPECT_EQ(reusable->GetCommandCount(), 3u);
        reusable->Execute();
        reusable->Reset();
        EXPECT_EQ(destructions, uint32_t(frame + 1) * 3);
    }
    EXPECT_EQ(DefaultAllocator::GetTrackedAllocationEvents(), allocations);
}

TEST_F(RHIExecutorTest, RecyclesPresentContextsAndReleasesProducerContextOnRhi)
{
    TestViewport viewport;
    RHICommandListPtr commands(
        RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));
    for (uint32_t frame = 0; frame < 16; ++frame)
    {
        commands->Draw(3, 1, 0, 0);
        const RHIBatchResult result = executor->SubmitFrame(*commands, &viewport).Wait();
        EXPECT_EQ(result.submission, RHISubmissionResult::eSuccess);
        EXPECT_TRUE(
            executor->WaitForSubmission(RHICommandContextType::eGraphics, result.serials[0]));
    }
    EXPECT_EQ(viewport.presents, 16u);
    EXPECT_EQ(rhi->contextCreations, 2u);
    EXPECT_EQ(rhi->graphics.proxyDestructions, 0u);
    commands.reset();
    const std::thread::id worker = GetRHIThread().Invoke([] { return std::this_thread::get_id(); });
    EXPECT_EQ(rhi->graphics.proxyDestructions, 1u);
    EXPECT_EQ(rhi->graphics.lastProxyDestroyThread, worker);
    executor->Destroy();
    EXPECT_EQ(rhi->graphics.proxyDestructions, 2u);
    EXPECT_EQ(rhi->graphics.lastProxyDestroyThread, worker);
}

TEST_F(RHIExecutorTest, DelayedGpuCompletionRetainsListsAndBoundsTheRetiredCache)
{
    TestViewport viewport;
    RHICommandListPtr commands(
        RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));
    constexpr uint32_t frameCount = RHIFrameState::kMaxFramesInFlight + 3;
    RHIBatchResult result;
    for (uint32_t frame = 0; frame < frameCount; ++frame)
    {
        commands->Draw(3, 1, 0, 0);
        result = executor->SubmitFrame(*commands, &viewport).Wait();
        EXPECT_EQ(result.submission, RHISubmissionResult::eSuccess);
    }
    commands.reset();
    executor->FlushRHIThread();
    EXPECT_EQ(rhi->contextCreations, frameCount + 1);
    EXPECT_EQ(rhi->graphics.proxyDestructions, 0u);
    EXPECT_TRUE(executor->WaitForSubmission(RHICommandContextType::eGraphics, result.serials[0]));
    EXPECT_EQ(rhi->contextCreations - rhi->graphics.proxyDestructions,
              RHIFrameState::kMaxFramesInFlight);
    executor->Destroy();
    EXPECT_EQ(rhi->graphics.proxyDestructions, rhi->contextCreations);
}

TEST(RHIThreadTest, FifoNestedInvokeExceptionsAndDrain)
{
    RHIThread thread;
    thread.Start(RHIExecutionMode::eThreaded, 2);
    HeapVector<uint32_t> order;
    for (uint32_t i = 0; i < 32; ++i)
    {
        thread.Dispatch([&order, i] { order.push_back(i); });
    }
    const std::thread::id worker = thread.Invoke([] { return std::this_thread::get_id(); });
    EXPECT_NE(worker, std::this_thread::get_id());
    EXPECT_EQ(thread.Invoke([&thread] { return thread.Invoke([] { return 17; }); }), 17);
    EXPECT_THROW(thread.Invoke([] { throw std::runtime_error("test RHI exception"); }),
                 std::runtime_error);
    thread.Stop();
    ASSERT_EQ(order.size(), 32u);
    for (uint32_t i = 0; i < 32; ++i)
    {
        EXPECT_EQ(order[i], i);
    }
}

TEST(RHIThreadTest, InlineUsesCallingThread)
{
    RHIThread thread;
    thread.Start(RHIExecutionMode::eInline);
    EXPECT_EQ(thread.Invoke([] { return std::this_thread::get_id(); }), std::this_thread::get_id());
    thread.Stop();
}

#if defined(ZEN_WIN32)
namespace
{
class RHIMessageWindow
{
public:
    RHIMessageWindow()
    {
        handle = CreateWindowExW(0, L"STATIC", L"RHI message test", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                 nullptr, GetModuleHandleW(nullptr), nullptr);
    }

    ~RHIMessageWindow()
    {
        if (handle != nullptr)
        {
            DestroyWindow(handle);
        }
    }

    void Send()
    {
        DWORD_PTR reply = 0;
        SetLastError(ERROR_SUCCESS);
        const LRESULT delivered =
            SendMessageTimeoutW(handle, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &reply);
        if (delivered != 0)
        {
            ++deliveredCount;
        }
        else
        {
            lastError = GetLastError();
        }
    }

    HWND handle{nullptr};
    uint32_t deliveredCount{0};
    DWORD lastError{ERROR_SUCCESS};
};

class RHIGatedWindowMessage
{
public:
    RHIGatedWindowMessage(RHIThread& thread, RHIMessageWindow& window) :
        m_thread(thread), m_window(window), m_release(m_open.get_future().share())
    {
        entered = m_entered.get_future();
    }

    ~RHIGatedWindowMessage()
    {
        Open();
        m_thread.Stop();
    }

    void Run()
    {
        m_entered.set_value();
        m_release.wait();
        m_window.Send();
    }

    void Open()
    {
        if (!m_opened)
        {
            m_opened = true;
            m_open.set_value();
        }
    }

    std::future<void> entered;

private:
    RHIThread& m_thread;
    RHIMessageWindow& m_window;
    std::promise<void> m_entered;
    std::promise<void> m_open;
    std::shared_future<void> m_release;
    bool m_opened{false};
};
} // namespace

TEST(RHIThreadTest, WindowsInvokeServicesSentMessagesAndPreservesPostedMessages)
{
    RHIMessageWindow window;
    ASSERT_NE(window.handle, nullptr);
    const UINT postedMessage = WM_APP + 31;
    ASSERT_TRUE(PostMessageW(window.handle, postedMessage, 71, 0));
    PostQuitMessage(19);
    RHIThread thread;
    thread.Start(RHIExecutionMode::eThreaded);
    thread.Invoke(&RHIMessageWindow::Send, &window);
    thread.Stop();
    EXPECT_EQ(window.deliveredCount, 1u) << "Win32 error " << window.lastError;

    // RHI waits must not consume the application's events or exit request.
    MSG message{};
    EXPECT_TRUE(PeekMessageW(&message, window.handle, postedMessage, postedMessage, PM_REMOVE));
    EXPECT_EQ(message.message, postedMessage);
    EXPECT_EQ(message.wParam, WPARAM(71));
    EXPECT_TRUE(PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE));
    EXPECT_EQ(message.message, UINT(WM_QUIT));
    EXPECT_EQ(message.wParam, WPARAM(19));
}

TEST(RHIThreadTest, WindowsFullQueueServicesSentMessagesBeforeAcceptingMoreWork)
{
    RHIMessageWindow window;
    ASSERT_NE(window.handle, nullptr);
    RHIThread thread;
    thread.Start(RHIExecutionMode::eThreaded, 1);
    RHIGatedWindowMessage gate(thread, window);
    thread.Dispatch(std::bind_front(&RHIGatedWindowMessage::Run, &gate));
    ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    HeapVector<uint32_t> order;
    thread.Dispatch([&order] { order.push_back(1); });
    gate.Open();
    // The worker cannot free this full FIFO until its sent window message completes.
    thread.Dispatch([&order] { order.push_back(2); });
    thread.Stop();
    EXPECT_EQ(window.deliveredCount, 1u) << "Win32 error " << window.lastError;
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], 1u);
    EXPECT_EQ(order[1], 2u);
}

TEST(RHIThreadTest, WindowsStopServicesSentMessagesWhileDrainingActiveAndQueuedWork)
{
    RHIMessageWindow window;
    ASSERT_NE(window.handle, nullptr);
    RHIThread thread;
    thread.Start(RHIExecutionMode::eThreaded, 1);
    RHIGatedWindowMessage gate(thread, window);
    thread.Dispatch(std::bind_front(&RHIGatedWindowMessage::Run, &gate));
    ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    thread.Dispatch(std::bind_front(&RHIMessageWindow::Send, &window));
    gate.Open();
    thread.Stop();
    EXPECT_EQ(window.deliveredCount, 2u) << "Win32 error " << window.lastError;
}

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
    const bool queued     = device->ExecuteRenderGraph(&viewport);
    const bool completed  = device->PollFrameSubmissions(true);
    GetRHIThread().Flush();
    rhi->beforeSubmission = {};
    EXPECT_TRUE(queued);
    EXPECT_TRUE(completed);
    EXPECT_EQ(window.deliveredCount, 2u) << "Win32 error " << window.lastError;
    EXPECT_EQ(viewport.presents, 1u);
}
#endif

TEST_F(RHIExecutorTest, DetachedLayoutAndResourcesSurviveCpuAndGpuDelay)
{
    RHITextureCreateInfo info{};
    info.format = DataFormat::eR8G8B8A8UNORM;
    info.type   = RHITextureType::e2D;
    info.width = info.height = 8;
    RHITexture* texture      = executor->CreateTexture(info);
    const uint64_t id        = texture->GetStableId();
    RHIRenderingLayout layout{};
    layout.SetRenderArea(0, 0, 8, 8);
    layout.AddColorRenderTarget(info.format, texture, RHIRenderTargetLoadOp::eClear,
                                RHIRenderTargetStoreOp::eStore);
    RHICommandListPtr commands(
        RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));
    commands->BeginRendering(&layout);
    commands->EndRendering();
    ArmGate();
    RHISubmissionTicket ticket = executor->SubmitFrame(*commands, nullptr);
    ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_FALSE(ticket.IsReady());
    EXPECT_EQ(commands->GetCommandCount(), 0u);
    commands->Reset();
    layout.SetRenderArea(0, 0, 1, 1);
    texture->ReleaseReference();
    EXPECT_FALSE(destroyed.contains(id));
    gate.Open();
    const RHIBatchResult result = ticket.Wait();
    EXPECT_EQ(result.submission, RHISubmissionResult::eSuccess);
    EXPECT_EQ(ToValue(gate.executionFrameState.GetFrameNumber()), 0u);
    EXPECT_EQ(ToIndex(gate.executionFrameState.GetFrameSlot()), 0u);
    EXPECT_NE(gate.executionThread, std::this_thread::get_id());
    EXPECT_TRUE(gate.releasedInTime);
    ASSERT_EQ(rhi->graphics.renderingLayouts.size(), 1u);
    EXPECT_EQ(rhi->graphics.renderingLayouts[0].renderArea.maxX, 8);
    EXPECT_FALSE(destroyed.contains(id));
    EXPECT_EQ(executor->GetLastCompletedSerial(RHICommandContextType::eGraphics), 0u);
    EXPECT_TRUE(executor->WaitForSubmission(RHICommandContextType::eGraphics, result.serials[0]));
    EXPECT_TRUE(destroyed.contains(id));
}

TEST_F(RHIExecutorTest, RejectionStopsAlreadyQueuedDependentBatches)
{
    RHICommandListPtr commands(
        RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));
    rhi->failSubmissionAt = 1;
    ArmGate();
    commands->Draw(3, 1, 0, 0);
    RHISubmissionTicket first = executor->SubmitFrame(*commands, nullptr);
    ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    commands->Draw(3, 1, 0, 0);
    RHISubmissionTicket second = executor->SubmitFrame(*commands, nullptr);
    gate.Open();
    EXPECT_EQ(first.Wait().submission, RHISubmissionResult::eRejected);
    EXPECT_EQ(second.Wait().submission, RHISubmissionResult::eFatal);
    executor->FlushRHIThread();
    EXPECT_TRUE(executor->AreSubmissionsBlocked());
    EXPECT_EQ(rhi->submissionAttempts, 1u);
    EXPECT_EQ(rhi->graphics.drawCount, 0u);
}

TEST_F(RHIExecutorTest, RollbackReleasesNewReferencesAndKeepsEarlierCommands)
{
    RHIBufferCreateInfo info{};
    info.size         = 64;
    RHIBuffer* first  = executor->CreateBuffer(info);
    RHIBuffer* second = executor->CreateBuffer(info);
    RHICommandListPtr commands(
        RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));
    commands->ClearBuffer(first, 0, 64);
    const RHICommandListBase::CommandCheckpoint checkpoint = commands->GetCommandCheckpoint();
    commands->CopyBuffer(first, second, RHIBufferCopyRegion{});
    EXPECT_EQ(first->GetRefCount(), 2u);
    EXPECT_EQ(second->GetRefCount(), 2u);
    commands->RollbackCommands(checkpoint);
    EXPECT_EQ(commands->GetCommandCount(), 1u);
    EXPECT_EQ(first->GetRefCount(), 2u);
    EXPECT_EQ(second->GetRefCount(), 1u);
    commands->Reset();
    EXPECT_EQ(first->GetRefCount(), 1u);
    first->ReleaseReference();
    second->ReleaseReference();
}

TEST_F(ThreadedRenderCoreTest, NextFrameCanBuildWhileRhiIsBlockedAndExtractionWaitsForConfirmation)
{
    RenderGraph* graph = device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph->Begin());
    RDGTexture texture = graph->GetResourceManager()->CreateTexture(LogicalTexture());
    graph->AddTransferPass("clear").ClearTexture(texture, Color(0.f));
    RDGExtractedTexture output = graph->GetResourceManager()->QueueTextureExtraction(texture);
    ASSERT_TRUE(graph->End());
    ArmGate();
    ASSERT_TRUE(device->ExecuteRenderGraph(&viewport));
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
    ASSERT_TRUE(device->ExecuteRenderGraph(&viewport));
    ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    gate.Open();
    device->FlushRHIThread();
    EXPECT_TRUE(device->AreSubmissionsBlocked());
    EXPECT_FALSE(output);
    EXPECT_FALSE(device->ExecuteRenderGraph(&viewport));
    EXPECT_EQ(rhi->submissionAttempts, 1u);
}

TEST_F(ThreadedRenderCoreTest, NextFrameRecordsCommandsBeforeWaitingForPriorSubmission)
{
    CreateTestShaderProgram(device, "intent");
    RenderGraph* graph            = device->GetCurrentFrameRDG();
    const RDGComputePassDesc pass = IntentPass();
    // Warm the pipeline cache and producer command list before measuring overlap.
    ASSERT_TRUE(graph->Begin());
    graph->AddComputePass(pass).RecordPassCommands(
        [](RDGPassCmdEncoder& encoder) { encoder.Dispatch(1, 1, 1); });
    ASSERT_TRUE(graph->End());
    ASSERT_TRUE(device->ExecuteRenderGraph(*graph));

    ASSERT_TRUE(graph->Begin());
    graph->AddComputePass(pass).RecordPassCommands(
        [](RDGPassCmdEncoder& encoder) { encoder.Dispatch(1, 1, 1); });
    ASSERT_TRUE(graph->End());
    ArmGate();
    ASSERT_TRUE(device->ExecuteRenderGraph(&viewport));
    ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    device->NextFrame();
    std::thread::id recordingThread;
    ASSERT_TRUE(graph->Begin());
    graph->AddComputePass(pass).RecordPassCommands(
        [this, &recordingThread](RDGPassCmdEncoder& encoder) {
            recordingThread = std::this_thread::get_id();
            encoder.Dispatch(1, 1, 1);
            gate.Open();
        });
    ASSERT_TRUE(graph->End());
    ASSERT_TRUE(device->ExecuteRenderGraph(&viewport));
    device->FlushRHIThread();
    EXPECT_TRUE(gate.releasedInTime);
    EXPECT_EQ(recordingThread, std::this_thread::get_id());
    EXPECT_NE(recordingThread, gate.executionThread);
    EXPECT_EQ(device->GetRHIThreadMetrics().completedBatches, 2u);
    EXPECT_EQ(device->GetRHIThreadMetrics().peakPendingBatches, 1u);
}

TEST_F(RHIExecutorTest, PartialFailureKeepsResourcesUntilDeviceDrain)
{
    RHITextureCreateInfo info{};
    RHITexture* texture = executor->CreateTexture(info);
    const uint64_t id   = texture->GetStableId();
    RHICommandListPtr commands(
        RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));
    commands->ClearTexture(texture, Color(0.f), RHITextureSubResourceRange());
    rhi->failSubmissionAt       = 1;
    rhi->submitBeforeFailure    = true;
    rhi->submissionFailure      = RHISubmissionResult::eFatal;
    RHISubmissionTicket ticket  = executor->SubmitFrame(*commands, nullptr);
    const RHIBatchResult result = ticket.Wait();
    EXPECT_EQ(result.submission, RHISubmissionResult::eFatal);
    EXPECT_GT(result.serials[0], 0u);
    texture->ReleaseReference();
    executor->WaitDeviceIdle();
    EXPECT_FALSE(destroyed.contains(id));
    commands.reset();
    executor->Destroy();
    EXPECT_TRUE(destroyed.contains(id));
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
        ASSERT_TRUE(device->ExecuteRenderGraph(&viewport));
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
    ASSERT_TRUE(device->ExecuteRenderGraph(&viewport));
    ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(ToValue(GRenderFrameState.GetFrameNumber()), 0u);
    EXPECT_EQ(ToValue(gate.executionFrameState.GetFrameNumber()), 1u);
    EXPECT_EQ(ToIndex(gate.executionFrameState.GetFrameSlot()), 1u);
    device->NextFrame();
    EXPECT_EQ(ToIndex(GRenderFrameState.GetFrameSlot()), 1u);
    gate.Open();
    device->FlushRHIThread();
    const uint64_t firstSerial =
        GDynamicRHI->GetLastSubmittedSerial(RHICommandContextType::eGraphics);
    ASSERT_GT(firstSerial, 0u);
    // Return to RenderCore slot 0 while the independent RHI timeline uses two slots.
    for (uint32_t i = 1; i < 3; ++i)
    {
        ASSERT_TRUE(graph->Begin());
        texture = graph->GetResourceManager()->CreateTexture(LogicalTexture());
        graph->AddTransferPass("clear").NeverCull().ClearTexture(texture, Color(0.f));
        ASSERT_TRUE(graph->End());
        ASSERT_TRUE(device->ExecuteRenderGraph(&viewport));
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

TEST_F(ThreadedRenderCoreTest, PooledNativeIdentityStaysStableDuringOverlappingRecording)
{
    CreateTestShaderProgram(device, "intent");
    RenderGraph* graph          = device->GetCurrentFrameRDG();
    RHIBuffer* physicalBuffer   = nullptr;
    RHITexture* physicalTexture = nullptr;
    uint64_t bufferId           = 0;
    uint64_t textureId          = 0;
    for (uint32_t phase = 0; phase < 3; ++phase)
    {
        ASSERT_TRUE(graph->Begin());
        const RDGBuffer buffer   = graph->GetResourceManager()->CreateBuffer(LogicalBuffer());
        const RDGTexture texture = graph->GetResourceManager()->CreateTexture(LogicalTexture());
        RDGComputePassDesc pass  = IntentPass();
        pass.BindStorageBuffer("write_buffer", buffer, RDGContentGuarantee::eFullWrite);
        pass.BindStorageImage("write_image", texture, RDGContentGuarantee::eFullWrite);
        if (phase == 2)
        {
            graph->AddComputePass(pass).RecordPassCommands([this, physicalBuffer, physicalTexture,
                                                            bufferId,
                                                            textureId](RDGPassCmdEncoder& encoder) {
                EXPECT_EQ(physicalBuffer->GetStableId(), bufferId);
                EXPECT_EQ(physicalTexture->GetStableId(), textureId);
                encoder.Dispatch(1, 1, 1);
                gate.Open();
            });
        }
        else
        {
            graph->AddComputePass(pass).RecordPassCommands(
                [](RDGPassCmdEncoder& encoder) { encoder.Dispatch(1, 1, 1); });
        }
        ASSERT_TRUE(graph->End());
        if (phase == 0)
        {
            // Warm the producer, pipeline, and physical allocation pool.
            ASSERT_TRUE(device->ExecuteRenderGraph(*graph));
            for (RHIResource* resource : rhi->graphics.boundResources)
            {
                if (resource->GetResourceType() == RHIResourceType::eBuffer)
                {
                    physicalBuffer = static_cast<RHIBuffer*>(resource);
                }
                else if (resource->GetResourceType() == RHIResourceType::eTextureView)
                {
                    physicalTexture = static_cast<RHITextureView*>(resource)->GetTexture();
                }
            }
            ASSERT_NE(physicalBuffer, nullptr);
            ASSERT_NE(physicalTexture, nullptr);
            bufferId  = physicalBuffer->GetStableId();
            textureId = physicalTexture->GetStableId();
        }
        else
        {
            if (phase == 1)
            {
                ArmGate();
            }
            ASSERT_TRUE(device->ExecuteRenderGraph(&viewport));
            EXPECT_EQ(DescribeResource(graph->GetResourceManager(), buffer).physicalStableId,
                      physicalBuffer->GetStableId());
            EXPECT_EQ(DescribeResource(graph->GetResourceManager(), texture).physicalStableId,
                      physicalTexture->GetStableId());
            if (phase == 1)
            {
                ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)),
                          std::future_status::ready);
                device->NextFrame();
            }
        }
    }
    device->FlushRHIThread();
    EXPECT_TRUE(gate.releasedInTime);
    EXPECT_EQ(physicalBuffer->GetStableId(), bufferId);
    EXPECT_EQ(physicalTexture->GetStableId(), textureId);
}

TEST_F(ThreadedRenderCoreTest, IdleCollectionRefreshesProgressAndRetiresBothKindsOfOwners)
{
    RHICommandListExecutor* executor = static_cast<RHICommandListExecutor*>(GDynamicRHI);
    RHITextureCreateInfo info{};
    RHITexture* batchOwned  = executor->CreateTexture(info);
    RHITexture* deviceOwned = executor->CreateTexture(info);
    const uint64_t batchId  = batchOwned->GetStableId();
    const uint64_t deviceId = deviceOwned->GetStableId();
    RHICommandListPtr commands(
        RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));
    commands->ClearTexture(batchOwned, Color(0.f), RHITextureSubResourceRange());
    commands->ClearTexture(deviceOwned, Color(0.f), RHITextureSubResourceRange());
    const RHIBatchResult result = executor->SubmitFrame(*commands, nullptr).Wait();
    ASSERT_GT(result.serials[0], 0u);
    batchOwned->ReleaseReference();
    device->DeferReleaseResource(deviceOwned);
    device->CollectCompletedResources();
    GetRHIThread().Flush();
    EXPECT_EQ(executor->GetLastCompletedSerial(RHICommandContextType::eGraphics), 0u);
    EXPECT_FALSE(destroyed.contains(batchId));
    EXPECT_FALSE(destroyed.contains(deviceId));

    // Complete after the last publication, without starting or submitting another frame.
    GetRHIThread().Invoke([this] { rhi->completed = rhi->submitted; });
    device->CollectCompletedResources();
    GetRHIThread().Flush(); // Only fence the worker; do not refresh through executor Flush.
    EXPECT_EQ(executor->GetLastCompletedSerial(RHICommandContextType::eGraphics),
              result.serials[0]);
    EXPECT_TRUE(destroyed.contains(batchId));
    device->CollectCompletedResources(); // Consume the published progress on RenderCore.
    GetRHIThread().Flush();
    EXPECT_TRUE(destroyed.contains(deviceId));
    EXPECT_TRUE(rhi->submissionWaits.empty());
    EXPECT_EQ(rhi->deviceIdleWaits, 0u);
}

TEST_F(ThreadedRenderCoreTest, FinalRhiReleaseRemovesBufferAndTextureHistory)
{
    RHITexture* texture      = Texture();
    RHIBuffer* buffer        = Buffer();
    const uint64_t textureId = texture->GetStableId();
    const uint64_t bufferId  = buffer->GetStableId();
    RenderGraph* graph       = device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph->Begin());
    graph->GetResourceManager()->ImportHostWrittenBuffer(buffer);
    graph->AddTransferPass("retire-history")
        .NeverCull()
        .ClearTexture(texture, Color(0.f))
        .CopyBuffer(buffer, buffer, {0, 32, 4});
    ASSERT_TRUE(graph->End());
    ArmGate();
    ASSERT_TRUE(device->ExecuteRenderGraph(&viewport));
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
    RHITexture* texture = Texture();
    const uint64_t id   = texture->GetStableId();
    RDGSubmissionTestAccess::Tracker(*device).UpdateTextureState(
        texture, RHIAccessMode::eReadWrite, RHITextureUsage::eTransferDst,
        BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eTransfer));
    RenderGraph* graph = device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph->Begin());
    graph->AddComputePass(IntentPass())
        .RecordPassCommands([this, texture, id](RDGPassCmdEncoder& encoder) {
            texture->ReleaseReference();
            device->CollectCompletedResources();
            EXPECT_TRUE(RDGSubmissionTestAccess::HasHistory(*device, id));
            encoder.Dispatch(1, 1, 1);
        });
    ASSERT_TRUE(graph->End());
    ArmGate();
    ASSERT_TRUE(device->ExecuteRenderGraph(&viewport));
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
    RHITexture* texture           = Texture();
    const uint64_t id             = texture->GetStableId();
    ResourceStateTracker& tracker = RDGSubmissionTestAccess::Tracker(*device);
    tracker.UpdateTextureState(
        texture, RHIAccessMode::eReadWrite, RHITextureUsage::eTransferDst,
        BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eTransfer));
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
    RHITexture* texture = Texture();
    const uint64_t id   = texture->GetStableId();
    RDGSubmissionTestAccess::Tracker(*device).UpdateTextureState(
        texture, RHIAccessMode::eReadWrite, RHITextureUsage::eTransferDst,
        BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eTransfer));
    RenderGraph graph("retirement_rollback");
    ASSERT_TRUE(graph.Begin());
    graph.AddComputePass(IntentPass())
        .RecordPassCommands([this, texture](RDGPassCmdEncoder& encoder) {
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

TEST_F(RHIExecutorTest, FlushSamplesGpuProgressWithoutWaitingForGpuCompletion)
{
    RHITextureCreateInfo info{};
    RHITexture* texture = executor->CreateTexture(info);
    const uint64_t id   = texture->GetStableId();
    RHICommandListPtr commands(
        RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));
    commands->ClearTexture(texture, Color(0.f), RHITextureSubResourceRange());
    const RHIBatchResult result = executor->SubmitFrame(*commands, nullptr).Wait();
    ASSERT_GT(result.serials[0], 0u);
    texture->ReleaseReference();
    executor->FlushRHIThread();
    EXPECT_EQ(executor->GetLastCompletedSerial(RHICommandContextType::eGraphics), 0u);
    EXPECT_FALSE(destroyed.contains(id));
    GetRHIThread().Invoke([this] { rhi->completed = rhi->submitted; });
    executor->FlushRHIThread();
    EXPECT_EQ(executor->GetLastCompletedSerial(RHICommandContextType::eGraphics),
              result.serials[0]);
    EXPECT_TRUE(destroyed.contains(id));
    EXPECT_TRUE(rhi->submissionWaits.empty());
    EXPECT_EQ(rhi->deviceIdleWaits, 0u);
}

TEST_F(RHIExecutorTest, ProgressPollsCoalesceWhileTheWorkerIsBusy)
{
    ArmGate();
    GetRHIThread().Dispatch(std::bind_front(&RHISubmissionGate::OnSubmission, &gate));
    ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    for (uint32_t poll = 0; poll < 128; ++poll)
    {
        executor->PollGPUProgress();
    }
    // A poll request must leave capacity for real work even when the worker is blocked.
    EXPECT_TRUE(GetRHIThread().TryDispatch([] {}));
    gate.Open();
    GetRHIThread().Flush();
    EXPECT_TRUE(gate.releasedInTime);
    EXPECT_FALSE(executor->AreSubmissionsBlocked());
}

TEST_F(RHIExecutorTest, ProgressPollRetriesAfterQueueSaturationWithoutBlocking)
{
    GetRHIThread().Invoke([this] { rhi->submitted[0] = rhi->completed[0] = 1; });
    ArmGate();
    GetRHIThread().Dispatch(std::bind_front(&RHISubmissionGate::OnSubmission, &gate));
    ASSERT_EQ(gate.entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    uint32_t queued = 0;
    while (queued < 128 && GetRHIThread().TryDispatch([] {}))
    {
        ++queued;
    }
    EXPECT_GT(queued, 0u);
    EXPECT_LT(queued, 128u);
    executor->PollGPUProgress();
    gate.Open();
    GetRHIThread().Flush();
    EXPECT_TRUE(gate.releasedInTime);
    // The failed enqueue must clear the pending flag so an idle caller can retry.
    GetRHIThread().Invoke([this] { rhi->submitted[0] = rhi->completed[0] = 2; });
    executor->PollGPUProgress();
    GetRHIThread().Flush();
    EXPECT_EQ(executor->GetLastCompletedSerial(RHICommandContextType::eGraphics), 2u);
    EXPECT_TRUE(rhi->submissionWaits.empty());
    EXPECT_EQ(rhi->deviceIdleWaits, 0u);
}

TEST_F(ThreadedRenderCoreTest, ProgressQueryFailureBlocksRetirementWithoutAPendingFrame)
{
    RHICommandListExecutor* executor = static_cast<RHICommandListExecutor*>(GDynamicRHI);
    RHITextureCreateInfo info{};
    RHITexture* texture = executor->CreateTexture(info);
    const uint64_t id   = texture->GetStableId();
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
    device->Destroy();
    EXPECT_TRUE(destroyed.contains(id));
}

TEST_F(ThreadedRenderCoreTest, BackendFailureRetainsBatchAndDeferredOwnersUntilShutdown)
{
    RHICommandListExecutor* executor = static_cast<RHICommandListExecutor*>(GDynamicRHI);
    RHITextureCreateInfo info{};
    RHITexture* batchOwned    = executor->CreateTexture(info);
    RHITexture* deferred      = executor->CreateTexture(info);
    const uint64_t batchId    = batchOwned->GetStableId();
    const uint64_t deferredId = deferred->GetStableId();
    RHICommandListPtr commands(
        RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));
    commands->ClearTexture(batchOwned, Color(0.f), RHITextureSubResourceRange());
    EXPECT_EQ(executor->SubmitFrame(*commands, nullptr).Wait().submission,
              RHISubmissionResult::eSuccess);
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
    device->Destroy();
    EXPECT_TRUE(destroyed.contains(batchId));
    EXPECT_TRUE(destroyed.contains(deferredId));
}
