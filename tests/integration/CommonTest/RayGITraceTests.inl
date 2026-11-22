TEST_P(RayQueryIntegrationTest, PipelineGITracePreservesRawSignalsGuidesAndReferenceSampling)
{
    if (SupportsHybridRayTracingPipeline(device->GetGPUInfo()) && device->GetGPUInfo().rayQuery.IsUsable())
    {
        ShaderProgramManager::GetInstance().BuildShaderPrograms(device);
        device->InitializeRendererServer();
        RendererServer&          server = *device->GetRendererServer();
        ForwardBlendSceneFixture fixture(*device);
        ASSERT_TRUE(fixture.Init(1, false, 2, true));
        ASSERT_TRUE(fixture.scene->SetVoxelBounds(sg::AABB(Vec3(-2, -2, -5), Vec3(2, 2, 0))));
        sg::MaterialData material     = fixture.scene->GetMaterialsData()[0];
        material.materialProperties.y = 0;
        material.metallicFactor       = 0;
        material.emissiveFactor       = Vec4(.5f, .25f, .125f, 1);
        ASSERT_TRUE(fixture.scene->UpdateMaterial(0, material));
        constexpr uint32_t side   = 65;
        constexpr uint32_t pixels = side * side;
        constexpr uint32_t frames = 8;
        rc::TextureFormat  format;
        format.width = format.height = side;
        format.depth                 = 1;
        format.format                = DataFormat::eR8G8B8A8UNORM;
        RHITexture* color            = device->CreateTextureColorRT(format, {}, "PipelineGITraceColor");
        format.format                = DataFormat::eD32SFloat;
        RHITexture* depth            = device->CreateTextureDepthStencilRT(format, {}, "PipelineGITraceDepth");
        textures.push_back(color);
        textures.push_back(depth);
        RHIFlags<RHIBufferUsageFlagBits> outputUsage;
        outputUsage.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer);
        constexpr uint32_t bytes   = pixels * ZEN_HYBRID_CAPTURE_BYTES_PER_PIXEL;
        RHIBuffer*         capture = Buffer(bytes, outputUsage, nullptr, RHIBufferAllocateType::eGPU);
        RHIBuffer* readback        = Buffer(bytes, RHIFlags<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferDstBuffer),
                                            nullptr, RHIBufferAllocateType::eCPURead);
        ASSERT_NE(color, nullptr);
        ASSERT_NE(depth, nullptr);
        ASSERT_NE(capture, nullptr);
        ASSERT_NE(readback, nullptr);
        const uint32_t countBytes = static_cast<uint32_t>(HybridGIRenderer::GetRayCountCaptureBytes(side, side));
        RHIBuffer*     counts     = Buffer(countBytes, outputUsage, nullptr, RHIBufferAllocateType::eGPU);
        RHIBuffer*     countReadback =
            Buffer(countBytes, RHIFlags<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferDstBuffer), nullptr,
                   RHIBufferAllocateType::eCPURead);
        ASSERT_NE(counts, nullptr);
        ASSERT_NE(countReadback, nullptr);
        DeferredLightingRenderer& renderer = *server.RequestDeferredLightingRenderer();
        renderer.SetHybridCapture(capture, readback);
        server.SetRenderOption(RenderOption::eVoxelGI);
        HeapVector<Vec4>       query(frames * pixels * ZEN_HYBRID_CAPTURE_COMPONENTS);
        uint32_t               maximumHalfULP      = 0;
        uint32_t               maximumCountHalfULP = 0;
        HeapVector<glm::uvec4> queryCounts(frames, glm::uvec4(0));
        for (uint32_t iteration = 0; iteration < 20; ++iteration)
        {
            SCOPED_TRACE(iteration);
            const uint32_t scenario       = iteration / 2;
            const bool     pipeline       = (iteration & 1u) != 0;
            const bool     counted        = scenario == 4 || scenario == 5;
            const bool     repeatPipeline = scenario == 6 || scenario == 7;
            const bool     resizeGuide    = scenario == 8;
            // Compare the first learned guide across providers while every receiver is still young.
            const bool             youngGuideParity = scenario == 9;
            VoxelGIRuntimeSettings settings         = server.GetVoxelGISettings();
            settings.resolution                     = 64;
            settings.cone.rayProvider               = VoxelGISettings::RayProvider::Hardware;
            settings.cone.rayTracingPipeline        = pipeline || repeatPipeline;
            settings.cone.pixelResolutionDivisor    = 2;
            settings.cone.restirGI                  = scenario == 2 || scenario == 7;
            settings.cone.referenceSamples          = scenario == 3 ? 64 : 0;
            // Cross-provider ULP differences can flip the age threshold. Replay identical RT
            // pipelines with young samples enabled to exercise deterministic guide accumulation.
            settings.cone.screenProbeGI        = false;
            settings.cone.worldRadianceCache   = false;
            settings.cone.youngReceiverSamples = repeatPipeline || youngGuideParity;
            ASSERT_TRUE(server.ApplyVoxelGISettings(settings));
            renderer.SetDeterministicGuideCapture(scenario == 2 || scenario == 3 || scenario == 5 || repeatPipeline
                                                  || youngGuideParity);
            ASSERT_TRUE(fixture.scene->SetEnvironmentLighting(scenario == 0 || resizeGuide ? 0 : .5f, 0, true, false));
            sg::MaterialData backdrop = fixture.scene->GetMaterialsData()[1];
            backdrop.roughnessFactor  = scenario == 2 ? .7f : .2f;
            ASSERT_TRUE(fixture.scene->UpdateMaterial(1, backdrop));
            RenderView view{color, depth, side, side};
            // Independent view histories start each query/pipeline comparison with the same sample sequence.
            view.historyId = 100 + iteration;
            if (counted)
            {
                ASSERT_EQ(server.DispatchRenderWorkloads(view), RenderFrameResult::eSubmitted);
                device->WaitForPreviousFrames();
                device->NextFrame();
                renderer.GetHybridGI()->SetRayCountCapture(counts, countReadback);
            }
            for (uint32_t frame = 0; frame < (youngGuideParity ? 2u : frames); ++frame)
            {
                SCOPED_TRACE(frame);
                if (resizeGuide)
                {
                    // Exercise no-guide placeholders, full diagnostic arrays and shrinking
                    // back to placeholders within one view's persistent history.
                    renderer.SetDeterministicGuideCapture(frame % 3 == 1);
                }
                ASSERT_EQ(server.DispatchRenderWorkloads(view), RenderFrameResult::eSubmitted);
                device->WaitForPreviousFrames();
                device->NextFrame();
                ASSERT_TRUE(renderer.GetHybridGI()->WasCaptureRecorded());
                ASSERT_EQ(renderer.GetHybridGI()->GetFrame(), frame + 1 + uint32_t(counted));
                const Vec4* values = reinterpret_cast<const Vec4*>(readback->Map());
                ASSERT_NE(values, nullptr);
                const uint32_t base = frame * pixels * ZEN_HYBRID_CAPTURE_COMPONENTS;
                if (!pipeline)
                {
                    std::memcpy(query.data() + base, values, bytes);
                }
                else
                {
                    uint32_t receivers       = 0;
                    uint32_t frameMaximumULP = 0;
                    for (uint32_t pixel = 0; pixel < pixels; ++pixel)
                    {
                        const uint32_t index  = pixel * ZEN_HYBRID_CAPTURE_COMPONENTS;
                        receivers            += values[index + 8].w > 0 ? 1u : 0u;
                        for (uint32_t component : {0u, 2u, 4u, 8u, 9u, 10u})
                        {
                            for (uint32_t channel = 0; channel < 4; ++channel)
                            {
                                const float value    = values[index + component][channel];
                                const float expected = query[base + index + component][channel];
                                if (component >= 8)
                                {
                                    EXPECT_EQ(value, expected) << pixel << ':' << component << ':' << channel;
                                }
                                else if (value != expected)
                                {
                                    EXPECT_TRUE(std::isfinite(value));
                                    EXPECT_TRUE(std::isfinite(expected));
                                    // Raw signals are RGBA16F: permit one storage ULP across shader stages.
                                    const uint32_t packedValue    = glm::packHalf2x16(Vec2(value, 0)) & 0xffffu;
                                    const uint32_t packedExpected = glm::packHalf2x16(Vec2(expected, 0)) & 0xffffu;
                                    const uint32_t distance =
                                        static_cast<uint32_t>(std::abs(int(packedValue) - int(packedExpected)));
                                    if (counted)
                                    {
                                        maximumCountHalfULP = std::max(maximumCountHalfULP, distance);
                                    }
                                    else
                                    {
                                        maximumHalfULP = std::max(maximumHalfULP, distance);
                                    }
                                    frameMaximumULP = std::max(frameMaximumULP, distance);
                                }
                            }
                        }
                    }
                    EXPECT_GT(receivers, 0u);
                    // Counter instrumentation changes floating-point scheduling; retain a
                    // four-ULP diagnostic tolerance and record its observed maximum separately.
                    EXPECT_LE(frameMaximumULP, repeatPipeline ? 0u : counted ? 4u : 1u);
                }
                readback->Unmap();
                if (counted)
                {
                    ASSERT_TRUE(renderer.GetHybridGI()->WereRayCountsRecorded());
                    const glm::uvec4* words = reinterpret_cast<const glm::uvec4*>(countReadback->Map());
                    ASSERT_NE(words, nullptr);
                    glm::uvec4 hardware(0), candidates(0);
                    for (uint32_t index = 0;
                         index < ((side + 15u) / 16u) * ((side + 15u) / 16u) * (ZEN_GI_TRACE_COUNT_WORDS / 4u);
                         index += ZEN_GI_TRACE_COUNT_WORDS / 4u)
                    {
                        EXPECT_EQ(words[index], glm::uvec4(0));
                        EXPECT_EQ(words[index + 2], glm::uvec4(0));
                        hardware   += words[index + 1];
                        candidates += words[index + 3];
                    }
                    const uint32_t pixelOffset = ((side + 15u) / 16u) * ((side + 15u) / 16u) * (ZEN_GI_TRACE_COUNT_WORDS / 4u);
                    glm::uvec4     breakdown(0);
                    for (uint32_t pixel = 0; pixel < side * side; ++pixel)
                    {
                        breakdown += words[pixelOffset + pixel * 2u];
                        EXPECT_EQ(words[pixelOffset + pixel * 2u + 1u], glm::uvec4(0));
                    }
                    EXPECT_EQ(breakdown.x + breakdown.y + breakdown.z + breakdown.w,
                              hardware.x + hardware.y + hardware.z + hardware.w);
                    countReadback->Unmap();
                    EXPECT_GT(hardware.x + hardware.y + hardware.z + hardware.w, 0u);
                    EXPECT_GT(candidates.x + candidates.y + candidates.z + candidates.w, 0u);
                    if (pipeline)
                    {
                        EXPECT_EQ(hardware, queryCounts[frame]);
                    }
                    else
                    {
                        queryCounts[frame] = hardware;
                    }
                }
                if (!repeatPipeline && !youngGuideParity)
                {
                    // Compare identical proposals for every sample sequence. Guide learning feeds
                    // tiny float differences back into later proposals, including query A/A runs.
                    // The final two cases verify exact guide repeatability through eight learned frames.
                    renderer.GetHybridGI()->InvalidateHistory("pipeline_sample_parity");
                }
            }
            renderer.GetHybridGI()->SetRayCountCapture(nullptr, nullptr);
        }
        renderer.SetHybridCapture(nullptr, nullptr);
        RecordProperty("max_raw_half_ulp", maximumHalfULP);
        RecordProperty("max_counted_raw_half_ulp", maximumCountHalfULP);
    }
    else
    {
        GTEST_SKIP() << "Ray-pipeline/query comparison requires both capabilities";
    }
}
