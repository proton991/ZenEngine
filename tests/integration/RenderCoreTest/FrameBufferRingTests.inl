namespace
{
class FrameBufferRingTest : public RenderCoreTest, public testing::WithParamInterface<uint32_t>
{
protected:
    void SetUp() override
    {
        InitializeDevice(nullptr, (GetParam() & 3) + 1,
                         (GetParam() & 4) != 0 ? RHIExecutionMode::eThreaded : RHIExecutionMode::eInline,
                         AsyncComputeMode::eDisabled, {false, true, {0, 1, 2}});
    }

    void FlushUploads()
    {
        RenderGraph graph("ring_uploads");
        ASSERT_TRUE(graph.Begin());
        ASSERT_TRUE(graph.End());
        ASSERT_TRUE(device->ExecuteRenderGraph(graph));
        device->WaitForPreviousFrames();
    }
};

TEST_P(FrameBufferRingTest, StableSlotsPreserveSparseEditsAndInvalidateOverwrittenGenerations)
{
    FrameBufferRing          ring;
    std::array<uint32_t, 64> data{};
    const uint32_t           slots = GRenderFrameState.GetNumFramesInFlight();
    RHIBuffer*               buffers[4]{};
    uint64_t                 generations[4]{};
    uint32_t                 warmedCreations = 0;
    for (uint32_t frame = 0; frame < slots * 4; ++frame)
    {
        data[0] = frame + 1;
        ring.MarkDirty(0, sizeof(uint32_t));
        if (frame == slots)
        {
            data[12] = 700;
            ring.MarkDirty(12 * sizeof(uint32_t), sizeof(uint32_t));
        }
        if (frame == slots + 1)
        {
            data[13] = 900;
            ring.MarkDirty(13 * sizeof(uint32_t), sizeof(uint32_t));
        }
        const uint32_t slot  = ToIndex(GRenderFrameState.GetFrameSlot());
        const uint64_t bytes = device->GetStagedUploadBytes();
        ASSERT_TRUE(ring.Update(*device, data.data(), sizeof(data), "ring_test"));
        FlushUploads();
        TestBuffer* current = static_cast<TestBuffer*>(ring.GetBuffer());
        EXPECT_EQ(std::memcmp(current->bytes.data(), data.data(), sizeof(data)), 0);
        if (frame >= slots)
        {
            EXPECT_EQ(current, buffers[slot]);
            EXPECT_FALSE(ring.ContainsGeneration(current, generations[slot]));
            EXPECT_EQ(rhi->bufferCreations, warmedCreations);
            EXPECT_LE(device->GetStagedUploadBytes() - bytes, 3 * sizeof(uint32_t));
        }
        buffers[slot]     = current;
        generations[slot] = ring.GetGeneration();
        EXPECT_TRUE(ring.ContainsGeneration(current, generations[slot]));
        if (frame == slots - 1)
        {
            warmedCreations = rhi->bufferCreations;
        }
        device->NextFrame();
    }
    // A frame with no edit keeps its contents and generation without an upload.
    const uint64_t generation = ring.GetGeneration();
    const uint64_t bytes      = device->GetStagedUploadBytes();
    ASSERT_TRUE(ring.Update(*device, data.data(), sizeof(data), "ring_test"));
    EXPECT_EQ(ring.GetGeneration(), generation);
    EXPECT_EQ(device->GetStagedUploadBytes(), bytes);
    ring.Destroy(*device);
    EXPECT_FALSE(ring.ContainsGeneration(buffers[0], generations[0]));
}

TEST_P(FrameBufferRingTest, GrowthFailureKeepsOldGenerationAndRetryPublishesCompleteData)
{
    FrameBufferRing           ring;
    std::array<uint32_t, 128> data{};
    ASSERT_TRUE(ring.Update(*device, data.data(), 64, "growing_ring", true));
    FlushUploads();
    RHIBuffer*     original   = ring.GetBuffer();
    const uint64_t generation = ring.GetGeneration();
    std::fill(data.begin(), data.end(), 31);
    ring.MarkDirty(0, sizeof(data));
    rhi->failBufferCreationAt = rhi->bufferCreations + 1;
    EXPECT_FALSE(ring.Update(*device, data.data(), sizeof(data), "growing_ring", true));
    EXPECT_EQ(ring.GetBuffer(), original);
    EXPECT_TRUE(ring.ContainsGeneration(original, generation));
    rhi->failBufferCreationAt = 0;
    ASSERT_TRUE(ring.Update(*device, data.data(), sizeof(data), "growing_ring", true));
    FlushUploads();
    EXPECT_NE(ring.GetBuffer(), original);
    // The grown copy goes to the next slot, so the original keeps its generation unless it is the only slot.
    EXPECT_EQ(ring.ContainsGeneration(original, generation), GRenderFrameState.GetNumFramesInFlight() > 1);
    EXPECT_EQ(std::memcmp(static_cast<TestBuffer*>(ring.GetBuffer())->bytes.data(), data.data(), sizeof(data)), 0);
    ring.Destroy(*device);
}

TEST_P(FrameBufferRingTest, UpdateAfterIdleFramesKeepsNewestGeneration)
{
    FrameBufferRing          ring;
    std::array<uint32_t, 16> data{};
    ASSERT_TRUE(ring.Update(*device, data.data(), sizeof(data), "idle_ring"));
    FlushUploads();
    RHIBuffer*     newest     = ring.GetBuffer();
    const uint64_t generation = ring.GetGeneration();
    // Return to the frame slot that wrote the newest generation without editing it.
    for (uint32_t frame = 0; frame < GRenderFrameState.GetNumFramesInFlight(); ++frame)
    {
        device->NextFrame();
    }
    data[0] = 1;
    ring.MarkDirty(0, sizeof(uint32_t));
    ASSERT_TRUE(ring.Update(*device, data.data(), sizeof(data), "idle_ring"));
    FlushUploads();
    EXPECT_NE(ring.GetGeneration(), generation);
    EXPECT_EQ(ring.ContainsGeneration(newest, generation), GRenderFrameState.GetNumFramesInFlight() > 1);
    ring.Destroy(*device);
}

INSTANTIATE_TEST_SUITE_P(FrameSlotsAndExecutionModes, FrameBufferRingTest, testing::Range(0u, 8u));
} // namespace
