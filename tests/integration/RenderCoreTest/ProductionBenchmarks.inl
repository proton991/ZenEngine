// Explicit opt-in: setup and resource retirement are outside the measured intervals.
TEST_F(RenderCoreTest, DISABLED_ProductionStateScalingBenchmark)
{
    using Clock  = std::chrono::steady_clock;
    using Access = RDGExecutionPlanTestAccess;

    RDGMetricsOptions options{};

    options.logging.enabled = false;

    options.validate        = false;

    device->GetRDGMetrics().Configure(options);

    StagingBufferManager staging;

    StagingUploadQueue uploads(device, &staging);

    HeapVector<RHITexture*> textures;

    RHITextureCreateInfo info{};

    info.format = DataFormat::eR8G8B8A8UNORM;

    info.type   = RHITextureType::e2D;

    // A complete 2048x1 chain has 12 mips without making CPU setup a bandwidth benchmark.
    info.width   = 2048;

    info.height  = 1;

    info.mipmaps = 12;

    info.usageFlags.SetFlags(RHITextureUsageFlagBits::eTransferDst, RHITextureUsageFlagBits::eTransferSrc);

    HeapVector<RHIBufferTextureCopyRegion> regions;

    uint32_t payloadSize = 0;

    for (uint32_t mip = 0; mip < info.mipmaps; ++mip)
    {
        RHIBufferTextureCopyRegion region{};

        region.bufferOffset = payloadSize;

        region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);

        region.textureSubresources.mipmap     = mip;

        region.textureSubresources.layerCount = 1;

        region.textureSize                    = {int32_t(info.width >> mip), 1, 1};

        regions.push_back(region);

        payloadSize += (info.width >> mip) * 4;
    }

    HeapVector<uint8_t> payload(payloadSize, 0x7f);

    RDGExecutor& executor            = Access::Executor(*device);

    RenderSubmissionHistory& history = RDGSubmissionTestAccess::History(*device);

    for (uint32_t count : {100u, 1000u, 5000u})
    {
        while (textures.size() < count)
        {
            RHITexture* texture = device->CreateTexture(info);

            ASSERT_NE(texture, nullptr);

            textures.push_back(texture);

            ASSERT_TRUE(uploads.EnqueueTexture(texture, regions, payloadSize, payload.data()));

            if (textures.size() % 128 == 0 || textures.size() == count)
            {
                ASSERT_TRUE(uploads.Flush());

                rhi->completed = rhi->submitted;

                device->CollectCompletedResources();

                uploads.ReclaimResources();
            }
        }

        for (RHITexture* texture : textures)
        {
            EXPECT_NE(history.Find(texture->GetStableId()), nullptr);

            const RDGResourceContent contents = executor.GetResourceStateTracker().GetContents(texture);

            EXPECT_EQ(contents.textureSubresources.size(), 12u);
        }

        RenderGraph graph("production_state_scaling");

        ASSERT_TRUE(graph.Begin());

        for (uint32_t index = 0; index < 4; ++index)
        {
            graph.AddTransferPass("touch").ClearTexture(textures[index], Color(0.f));
        }

        ASSERT_TRUE(graph.End());

        RHICommandList commands;

        HeapVector<double> execution, resolution, trackerCopy, metricsCopy;

        for (uint32_t run = 0; run < 35; ++run)
        {
            Access::Plan plan;

            ASSERT_TRUE(Access::Prepare(executor, graph, plan));

            const Clock::time_point start = Clock::now();

            const bool executed           = Access::Execute(executor, plan, commands);

            const Clock::time_point end   = Clock::now();

            ASSERT_TRUE(executed);

            commands.Reset();

            const Clock::time_point resolveStart = Clock::now();

            const bool resolved                  = history.ResolveAccepted();

            const Clock::time_point resolveEnd   = Clock::now();

            ASSERT_TRUE(resolved);

            if (run >= 5)
            {
                execution.push_back(std::chrono::duration<double, std::micro>(end - start).count());

                resolution.push_back(std::chrono::duration<double, std::micro>(resolveEnd - resolveStart).count());
            }
        }

        // The optional whole-scene copy probe evicts the working set and churns the allocator.
        // Keep it out of the repeated small-graph measurements, especially after removing that copy.
        for (uint32_t run = 0; run < 35; ++run)
        {
            const Clock::time_point trackerStart = Clock::now();

            {
                ResourceStateTracker copy = executor.GetResourceStateTracker();

                EXPECT_EQ(copy.GetRevision(), executor.GetResourceStateTracker().GetRevision());
            }

            const Clock::time_point trackerEnd   = Clock::now();

            const Clock::time_point metricsStart = Clock::now();

            {
                RDGMetrics copy = executor.GetMetrics();

                EXPECT_FALSE(copy.GetOptions().validate);
            }

            const Clock::time_point metricsEnd = Clock::now();

            if (run >= 5)
            {
                trackerCopy.push_back(std::chrono::duration<double, std::micro>(trackerEnd - trackerStart).count());

                metricsCopy.push_back(std::chrono::duration<double, std::micro>(metricsEnd - metricsStart).count());
            }
        }

        std::sort(execution.begin(), execution.end());

        std::sort(resolution.begin(), resolution.end());

        std::sort(trackerCopy.begin(), trackerCopy.end());

        std::sort(metricsCopy.begin(), metricsCopy.end());

        std::printf(
            "PRODUCTION_STATE n=%u mips=12 touched=4 samples=30 transaction_us=%.3f resolve_us=%.3f tracker_copy_us=%.3f metrics_copy_us=%.3f transaction_min_us=%.3f transaction_p95_us=%.3f\n",
            count, execution[15], resolution[15], trackerCopy[15], metricsCopy[15], execution[0], execution[28]);
    }

    uploads.Destroy();

    staging.Destroy();

    for (RHITexture* texture : textures)
    {
        device->DestroyTexture(texture);
    }
}
