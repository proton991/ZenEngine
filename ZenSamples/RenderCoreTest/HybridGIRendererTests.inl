void CreateHybridTestShaders(RenderDevice* device)
{
    for (const std::array<const char*, 2>& shader :
         {std::array<const char*, 2>{"HybridTraceSP", "VoxelGI/hybrid_trace.comp.spv"},
          {"HybridTraceCountsSP", "VoxelGI/hybrid_trace_counts.comp.spv"},
          {"EnvironmentColumnsSP", "VoxelGI/environment_columns.comp.spv"},
          {"EnvironmentRowsSP", "VoxelGI/environment_rows.comp.spv"},
          {"EnvironmentTilesSP", "VoxelGI/environment_tiles.comp.spv"},
          {"HybridTraceHalfSP", "VoxelGI/hybrid_trace_half.comp.spv"},
          {"HybridTraceHalfCountsSP", "VoxelGI/hybrid_trace_half_counts.comp.spv"},
          {"HybridTraceFallbackSP", "VoxelGI/hybrid_trace_fallback.comp.spv"},
          {"HybridTraceFallbackCountsSP", "VoxelGI/hybrid_trace_fallback_counts.comp.spv"},
          {"HybridDiffuseUpsampleSP", "VoxelGI/hybrid_diffuse_upsample.comp.spv"},
          {"HybridTemporalSP", "VoxelGI/hybrid_temporal.comp.spv"},
          {"HybridTemporalReferenceSP", "VoxelGI/hybrid_temporal_reference.comp.spv"},
          {"HybridTemporalCachedSP", "VoxelGI/hybrid_temporal_cached.comp.spv"},
          {"HybridFilterPrepareSP", "VoxelGI/hybrid_filter_prepare.comp.spv"},
          {"HybridFilter0SP", "VoxelGI/hybrid_filter_cached.comp.spv"},
          {"HybridFilter1SP", "VoxelGI/hybrid_filter_cached.comp.spv"},
          {"HybridFilter2SP", "VoxelGI/hybrid_filter.comp.spv"},
          {"HybridFilter3SP", "VoxelGI/hybrid_filter.comp.spv"},
          {"HybridFilter4SP", "VoxelGI/hybrid_filter_final.comp.spv"},
          {"HybridSkySupportSP", "VoxelGI/hybrid_sky_support.comp.spv"},
          {"HybridSkySearchSP", "VoxelGI/hybrid_sky_search.comp.spv"},
          {"VoxelFilterAlbedoSP", "VoxelGI/filter_albedo.comp.spv"},
          {"VoxelFilterRadianceSP", "VoxelGI/filter_radiance.comp.spv"},
          {"VoxelSkyIrradianceLegacySP", "VoxelGI/sky_irradiance_legacy.comp.spv"},
          {"VoxelInjectRadianceSP", "VoxelGI/inject_radiance.comp.spv"}})
    {
        RHIShaderCreateInfo info;
        info.stageFlags.SetFlag(RHIShaderStageFlagBits::eCompute);
        info.spirvFileName[ToUnderlying(RHIShaderStage::eCompute)] = shader[1];
        reflectedShaderInfos[shader[0]]                            = info;
        CreateTestShaderProgram(device, shader[0]);
    }
    RHIShaderCreateInfo receiverInfo;
    receiverInfo.stageFlags.SetFlags(RHIShaderStageFlagBits::eVertex, RHIShaderStageFlagBits::eFragment);
    receiverInfo.spirvFileName[ToUnderlying(RHIShaderStage::eVertex)]   = "SceneRenderer/receiver.vert.spv";
    receiverInfo.spirvFileName[ToUnderlying(RHIShaderStage::eFragment)] = "SceneRenderer/receiver.frag.spv";
    reflectedShaderInfos["HybridReceiverSP"]                            = receiverInfo;
    CreateTestShaderProgram(device, "HybridReceiverSP");
}

TEST(HybridGIRenderer, SkyLightNeedsEnvironmentLightingAndALitEnvironment)
{
    VoxelGISettings  settings;
    SceneUniformData scene;
    settings.environmentLighting = true;
    scene.environment            = Vec4(1.0f, 0.0f, 1.0f, 1.0f);
    EXPECT_TRUE(HybridGIRenderer::HasSkyLight(settings, scene));
    // The skybox's visibility does not light anything.
    scene.environment.w = 0.0f;
    EXPECT_TRUE(HybridGIRenderer::HasSkyLight(settings, scene));
    scene.environment.z = 0.0f;
    EXPECT_FALSE(HybridGIRenderer::HasSkyLight(settings, scene));
    scene.environment = Vec4(0.0f, 0.0f, 1.0f, 1.0f);
    EXPECT_FALSE(HybridGIRenderer::HasSkyLight(settings, scene));
    scene.environment            = Vec4(1.0f, 0.0f, 1.0f, 1.0f);
    settings.environmentLighting = false;
    EXPECT_FALSE(HybridGIRenderer::HasSkyLight(settings, scene));
}

TEST_F(RenderCoreTest, HybridHistoriesPublishOnlySuccessfulFramesAndResizePerView)
{
    CreateHybridTestShaders(device);
    TestViewport            viewport;
    HeapVector<RHIBuffer*>  ownedBuffers;
    HeapVector<RHITexture*> ownedTextures;
    AllocateRendererInputs(0, viewport, ownedBuffers, ownedTextures);
    sceneInputs.camera.projViewMatrix = Mat4(1);
    sceneInputs.camera.proj           = Mat4(1);
    sceneInputs.camera.view           = Mat4(1);
    sg::Scene source;
    source.GetAABB() = sg::AABB(Vec3(-1), Vec3(1));
    SceneData data{};
    data.pScene = &source;
    RenderScene      scene(device, data);
    TestVoxelVolumes volumes(device, DataFormat::eR8G8B8A8UNORM);
    volumes.Init();
    volumes.SetRenderScene(&scene);
    VoxelGIRenderer gi(device, &volumes);
    gi.SetRenderScene(&scene);
    ASSERT_TRUE(gi.Init());
    // Establish the test's starting state independently of engine.cfg and earlier config tests.
    VoxelGISettings bounceSettings;
    bounceSettings.bounceSource = VoxelGISettings::BounceSource::Rays;
    // Exercise reflected-hit histories on the compute-only test device.
    bounceSettings.reflections = VoxelGISettings::Reflections::On;
    // Opt into half-resolution diffuse to exercise its trace, upsample and fallback passes.
    bounceSettings.diffuseResolution = VoxelGISettings::DiffuseResolution::Half;
    ASSERT_TRUE(gi.SetSettings(bounceSettings));
    DeferredLightingRenderer renderer(device);
    renderer.Init();
    renderer.SetRenderScene(&scene);
    RenderView             view  = RenderView::FromViewport(viewport);
    HybridGIRenderer*      first = nullptr;
    HeapVector<RHIBuffer*> vertexGenerations;
    vertexGenerations.push_back(sceneInputs.vertices);
    for (uint32_t attempt = 0; attempt < 25; ++attempt)
    {
        const uint32_t frame                      = attempt == 0 ? 0 : attempt <= 7 ? attempt - 1 : std::min(attempt - 2, 21u);

        const bool declined                       = attempt == 0 || attempt == 7 || attempt == 23;

        const uint64_t previousRadianceGeneration = gi.GetRadianceGeneration();

        SCOPED_TRACE(attempt);
        RenderGraph* graph = device->GetCurrentFrameRDG();
        ASSERT_TRUE(graph->Begin());
        if (frame == 0)
        {
            for (RHITexture* texture : ownedTextures)
            {
                if (texture != viewport.depth)
                {
                    graph->AddTransferPass("InitializeHybridTestInput").ClearTexture(texture, Color(0));
                }
            }
        }
        if (volumes.BeginVolumeUpdate(*graph))
        {
            for (RHITexture* texture :
                 {volumes.GetVoxelTextures().pAlbedo, volumes.GetVoxelTextures().pNormal, volumes.GetVoxelTextures().pEmissive})
            {
                graph->AddTransferPass("InitializeHybridVoxels").ClearTexture(texture, Color(0));
            }
        }
        view.historyId = frame == 3 ? 1 : 0;
        if (frame == 4)
        {
            view.width *= 2;
        }
        if (frame == 2 || frame == 6)
        {
            VoxelGISettings changed = gi.GetSettings();
            changed.samples         = frame == 2 ? 2 : 4;
            ASSERT_TRUE(gi.SetSettings(changed));
        }
        if (frame == 7)
        {
            ASSERT_TRUE(scene.SetEnvironmentLighting(0.5f, 30.0f, true, false));
        }
        if (frame == 8)
        {
            renderer.SetRenderScene(&scene);
        }
        if (frame == 9 || frame == 11)
        {
            VoxelGISettings changed   = gi.GetSettings();
            changed.analyticLighting  = false;
            changed.indirectIntensity = frame == 11 ? .5f : 1.0f;
            ASSERT_TRUE(gi.SetSettings(changed));
        }
        if (frame == 12)
        {
            // Cone bounce shrinks the bounce histories to placeholders.
            VoxelGISettings changed = gi.GetSettings();
            changed.bounceSource    = VoxelGISettings::BounceSource::Cone;
            ASSERT_TRUE(gi.SetSettings(changed));
        }
        if (frame == 13 || frame == 14)
        {
            VoxelGISettings changed  = gi.GetSettings();
            changed.reflections      = frame == 14 ? VoxelGISettings::Reflections::On : VoxelGISettings::Reflections::Off;
            changed.analyticLighting = frame == 14;
            ASSERT_TRUE(gi.SetSettings(changed));
        }
        if (frame == 15)
        {
            VoxelGISettings changed  = gi.GetSettings();
            changed.adaptiveSampling = false;
            ASSERT_TRUE(gi.SetSettings(changed));
        }
        if (frame == 16)
        {
            // Ordinary camera motion keeps temporal history, but same-pixel
            // geometry weights and settled sampling budgets must not reuse it.
            sceneInputs.camera.projViewMatrix[3][0] += 0.01f;
        }
        if (frame == 18 || frame == 20)
        {
            // Enter and leave FP32 reference history without keeping samples from the replaced format.
            VoxelGISettings changed  = gi.GetSettings();
            changed.referenceSamples = frame == 18 ? 256 : 0;
            ASSERT_TRUE(gi.SetSettings(changed));
        }
        if (frame >= 1 && frame <= 3)
        {
            // Replace the immutable vertex generation without replacing the scene facade.
            sceneInputs.vertices = Buffer();
            ownedBuffers.push_back(sceneInputs.vertices);
            vertexGenerations.push_back(sceneInputs.vertices);
            scene.PrepareBuffers();
        }
        renderer.BuildGBufferGraph(view, true);
        HybridGIRenderer* hybrid = renderer.GetHybridGI();
        if (frame == 15)
        {
            RHIBufferCreateInfo info;
            info.size         = HybridGIRenderer::GetRayCountCaptureBytes(view.width, view.height);
            info.allocateType = RHIBufferAllocateType::eGPU;
            info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer);
            RHIBuffer* counts = device->CreateBuffer(info);
            ASSERT_NE(counts, nullptr);
            ownedBuffers.push_back(counts);
            info.allocateType = RHIBufferAllocateType::eCPURead;
            info.usageFlags   = 0;
            info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);
            RHIBuffer* readback = device->CreateBuffer(info);
            ASSERT_NE(readback, nullptr);
            ownedBuffers.push_back(readback);
            hybrid->SetRayCountCapture(counts, readback);
        }
        if (frame <= 4)
        {
            // Frame 1 fails, so frame 2 must restart from its current vertices. The second
            // view at frame 3 must not replace the first view's successful frame-2 state.
            const uint32_t  previousGeneration[5] = {0, 0, 2, 3, 2};
            RDGPassDescBase receiver;
            hybrid->BindReceiverInputs(receiver);
            ASSERT_EQ(receiver.bufferStorage.size(), 1u);
            EXPECT_EQ(receiver.bufferStorage[0], vertexGenerations[previousGeneration[frame]]);
        }
        if (frame == 0)
        {
            first = hybrid;
        }
        if (frame == 3)
        {
            EXPECT_NE(first, hybrid);
        }
        else
        {
            EXPECT_EQ(first, hybrid);
        }
        if (frame == 14)
        {
            // Re-enabling reflections expands the cache snapshot; fail just that allocation.
            rhi->failTextureCreationAt = rhi->textureCreations + 1;
        }
        gi.BuildRayQueryGraph();
        gi.BuildRenderGraph();
        rhi->failTextureCreationAt = 0;
        EXPECT_EQ(gi.HasPreviousRadiance(), frame != 14);
        const bool historyBeforeBuild = hybrid->IsHistoryValid();

        ASSERT_TRUE(hybrid->BuildRenderGraph(view, gi));
        EXPECT_EQ(hybrid->WereRayCountsRecorded(), frame >= 15);
        EXPECT_EQ(hybrid->IsHistoryValid(),
                  frame == 1 || frame == 5 || (frame >= 9 && frame != 15 && frame != 18 && frame != 20));
        if (frame == 2 || frame == 6 || frame == 15 || frame == 18 || frame == 20)
        {
            EXPECT_STREQ(hybrid->GetResetReason(), "settings");
        }
        if (frame == 7)
        {
            EXPECT_STREQ(hybrid->GetResetReason(), "environment");
        }
        // Compute traces diffuse at half resolution only with valid history, outside reference mode.
        const bool half = hybrid->IsHistoryValid() && frame != 18 && frame != 19;
        EXPECT_EQ(hybrid->UsesHalfResolutionDiffuse(), half);
        const uint32_t before = hybrid->GetFrame();
        ASSERT_TRUE(graph->End()) << graph->GetResult().message;
        const bool accepted = frame != 1 && !declined;
        if (accepted)
        {
            const size_t firstValue    = rhi->graphics.values.size();
            const size_t firstDispatch = rhi->graphics.dispatches.size();
            const size_t firstIndirect = rhi->graphics.indirectShaders.size();
            ASSERT_TRUE(device->ExecuteRenderGraph(*graph)) << graph->GetResult().message;
            uint32_t         referenceDispatches = 0;
            uint32_t         fullTraces = 0, halfTraces = 0, upsamples = 0, fallbacks = 0;
            const glm::uvec3 guideGroups = glm::uvec3((view.width + 15u) / 16u, (view.height + 15u) / 16u, 1u);
            for (size_t index = firstDispatch; index < rhi->graphics.dispatches.size(); ++index)
            {
                const std::pair<RHIShader*, glm::uvec3>& dispatch = rhi->graphics.dispatches[index];
                const std::string                        shader =
                    dispatch.first != nullptr
                        ? dispatch.first->GetCreateInfo().spirvFileName[ToUnderlying(RHIShaderStage::eCompute)]
                        : std::string();
                if (shader == "VoxelGI/hybrid_temporal_reference.comp.spv")
                {
                    EXPECT_EQ(dispatch.second, glm::uvec3((view.width + 7u) / 8u, (view.height + 7u) / 8u, 1u));
                    ++referenceDispatches;
                }
                // The trace runs one group per guide tile; the upsample one per 8x8 pixels.
                if (shader == "VoxelGI/hybrid_trace.comp.spv" || shader == "VoxelGI/hybrid_trace_counts.comp.spv")
                {
                    EXPECT_EQ(dispatch.second, guideGroups);
                    ++fullTraces;
                }
                if (shader == "VoxelGI/hybrid_trace_half.comp.spv" || shader == "VoxelGI/hybrid_trace_half_counts.comp.spv")
                {
                    EXPECT_EQ(dispatch.second, guideGroups);
                    ++halfTraces;
                }
                if (shader == "VoxelGI/hybrid_diffuse_upsample.comp.spv")
                {
                    EXPECT_EQ(dispatch.second, glm::uvec3((view.width + 7u) / 8u, (view.height + 7u) / 8u, 1u));
                    ++upsamples;
                }
            }
            // The fallback pass is dispatched indirectly over the upsample's frame-wide list.
            for (size_t index = firstIndirect; index < rhi->graphics.indirectShaders.size(); ++index)
            {
                RHIShader* const  shader = rhi->graphics.indirectShaders[index];
                const std::string file =
                    shader != nullptr ? shader->GetCreateInfo().spirvFileName[ToUnderlying(RHIShaderStage::eCompute)] : "";
                if (file == "VoxelGI/hybrid_trace_fallback.comp.spv" || file == "VoxelGI/hybrid_trace_fallback_counts.comp.spv")
                {
                    ++fallbacks;
                }
            }
            EXPECT_EQ(referenceDispatches, frame == 18 || frame == 19 ? 1u : 0u);
            EXPECT_EQ(fullTraces, half ? 0u : 1u);
            EXPECT_EQ(halfTraces, half ? 1u : 0u);
            EXPECT_EQ(upsamples, half ? 1u : 0u);
            EXPECT_EQ(fallbacks, half ? 1u : 0u);
            bool     foundUniforms = false;
            uint32_t reuseValues   = 0;
            for (size_t value = firstValue; value < rhi->graphics.values.size(); ++value)
            {
                if (rhi->graphics.valueBindings[value] == glm::uvec2(1, 28))
                {
                    ASSERT_EQ(rhi->graphics.values[value].size(), sizeof(glm::uvec4));
                    glm::uvec4 reuse;
                    std::memcpy(&reuse, rhi->graphics.values[value].data(), sizeof(reuse));
                    const uint32_t expected =
                        frame == 5 || (frame >= 9 && frame <= 14) || frame == 17 || frame == 19 || frame == 21 ? 1u : 0u;
                    // The environment stays lit throughout, so the dense sky search stays enabled.
                    EXPECT_EQ(reuse, glm::uvec4(frame == 19 ? 0u : expected, expected, 1u, 0u));
                    ++reuseValues;
                }
                if (rhi->graphics.values[value].size() == sizeof(HybridGIUniformData))
                {
                    HybridGIUniformData uniforms;
                    std::memcpy(&uniforms, rhi->graphics.values[value].data(), sizeof(uniforms));
                    EXPECT_EQ(uniforms.sampling.w, hybrid->IsHistoryValid() ? 1u : 0u);
                    EXPECT_EQ(uniforms.rejection.w, frame == 18 || frame == 19 ? 1.0f : 0.0f);
                    // Only steady history and a settings reset after a successful frame can reuse the guide.
                    // Startup, failed-frame recovery, a new view, resize, environment and scene resets cannot.
                    EXPECT_EQ(uniforms.provider.y, frame == 5 || frame == 6 || frame >= 9 ? 1u : 0u);
                    EXPECT_EQ(uniforms.provider.w, frame < 2 || (frame >= 6 && frame < 15) ? 1u : 0u);
                    EXPECT_EQ(uniforms.bounce.x, frame >= 12 ? 0u : 1u);
                    EXPECT_EQ(uniforms.reflection.x, frame == 13 ? 0u : 1u);
                    if (frame >= 9)
                    {
                        // An injection change keeps bounce history for the temporal pass to test; intensity
                        // and source changes reset it. None of them touches sky.
                        EXPECT_EQ(uniforms.sampling.w, frame == 15 || frame == 18 || frame == 20 ? 0u : 1u);
                        EXPECT_EQ(uniforms.bounce.y,
                                  frame == 11 || frame == 12 || frame == 14 || frame == 15 || frame == 18 || frame == 20 ? 0u
                                                                                                                         : 1u);
                        EXPECT_EQ(uniforms.bounce.z, 1u);
                        // Only the injection frame measures the change between the caches.
                        EXPECT_EQ(uniforms.bounce.w, frame == 9 ? 1u : 0u);
                        EXPECT_EQ(uniforms.reflection.y,
                                  frame == 11 || frame == 13 || frame == 14 || frame == 15 || frame == 18 || frame == 20 ? 0u
                                                                                                                         : 1u);
                        EXPECT_EQ(uniforms.reflection.w, frame == 9 ? 1u : 0u);
                    }
                    foundUniforms = true;
                }
            }
            EXPECT_TRUE(foundUniforms);
            // The trace, the half-resolution fallback and the final filter bind it; reference captures skip the filter.
            EXPECT_EQ(reuseValues, frame == 18 || frame == 19 ? 1u : half ? 3u : 2u);
        }
        const RenderFrameResult result = declined ? RenderFrameResult::eDeclined
                                       : accepted ? RenderFrameResult::eSubmitted
                                                  : RenderFrameResult::eFailed;

        renderer.OnRenderGraphExecuted(result);

        gi.OnRenderGraphExecuted(result);

        volumes.OnRenderGraphExecuted(result);

        if (declined)
        {
            EXPECT_EQ(hybrid->IsHistoryValid(), historyBeforeBuild);

            EXPECT_EQ(gi.GetRadianceGeneration(), previousRadianceGeneration);
        }
        EXPECT_EQ(hybrid->GetFrame(), before + (accepted ? 1u : 0u));
        if (!accepted && !declined)
        {
            EXPECT_STREQ(hybrid->GetResetReason(), "failed_frame");
        }
        if (frame == 4)
        {
            EXPECT_STREQ(hybrid->GetResetReason(), "resize");
        }
    }
    FrameBufferRing          ring;
    std::array<uint32_t, 16> ringData{};
    RHIBuffer*               previous = nullptr;
    sceneInputs.vertexRing            = &ring;
    for (uint32_t step = 0; step < 5; ++step)
    {
        device->NextFrame();
        ringData[0] = step;
        ring.MarkDirty(0, sizeof(uint32_t));
        ASSERT_TRUE(ring.Update(*device, ringData.data(), sizeof(ringData), "hybrid_vertices", true));
        sceneInputs.vertices = ring.GetBuffer();
        scene.PrepareBuffers();
        RenderGraph& graph = *device->GetCurrentFrameRDG();
        ASSERT_TRUE(graph.Begin());
        // Skip this view while another consumer updates both ring slots.
        const bool render = step < 2 || step == 4;
        if (render)
        {
            renderer.BuildGBufferGraph(view, true);
            HybridGIRenderer* hybrid = renderer.GetHybridGI();
            EXPECT_EQ(hybrid->IsHistoryValid(), step == 1);
            if (step == 4)
            {
                EXPECT_STREQ(hybrid->GetResetReason(), "vertex_ring_wrapped");
            }
            RDGPassDescBase receiver;
            hybrid->BindReceiverInputs(receiver);
            ASSERT_EQ(receiver.bufferStorage.size(), 1u);
            EXPECT_EQ(receiver.bufferStorage[0], step == 1 ? previous : ring.GetBuffer());
            gi.BuildRayQueryGraph();
            gi.BuildRenderGraph();
            ASSERT_TRUE(hybrid->BuildRenderGraph(view, gi));
        }
        ASSERT_TRUE(graph.End()) << graph.GetResult().message;
        if (step != 4)
        {
            ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
        }
        if (render)
        {
            const RenderFrameResult result = step == 4 ? RenderFrameResult::eDeclined : RenderFrameResult::eSubmitted;
            renderer.OnRenderGraphExecuted(result);
            gi.OnRenderGraphExecuted(result);
            volumes.OnRenderGraphExecuted(result);
            EXPECT_EQ(renderer.GetHybridGI()->IsHistoryValid(), step != 4);
        }
        previous = ring.GetBuffer();
    }
    renderer.Destroy();
    sceneInputs.vertexRing = nullptr;
    ring.Destroy(*device);
    gi.Destroy();
    volumes.Destroy();
    for (RHIBuffer* buffer : ownedBuffers)
    {
        device->DestroyBuffer(buffer);
    }
    for (RHITexture* texture : ownedTextures)
    {
        device->DestroyTexture(texture);
    }
    sceneInputs = {};
}

class DelayedRadianceTest : public RenderCoreTest, public testing::WithParamInterface<uint32_t>
{
protected:
    void SetUp() override
    {
        const RHIExecutionMode execution = GetParam() < 2 ? RHIExecutionMode::eInline : RHIExecutionMode::eThreaded;

        const AsyncComputeMode compute   = GetParam() % 2 == 0 ? AsyncComputeMode::eDisabled : AsyncComputeMode::eAuto;

        InitializeDevice(nullptr, 2, execution, compute, {true, true, {0, 1, 2}});

        CreateHybridTestShaders(device);

        RDGMetricsOptions metrics    = device->GetRDGMetrics().GetOptions();

        metrics.maxNodeDetails       = 512;

        metrics.includeTransferNodes = true;

        device->GetRDGMetrics().Configure(metrics);

        device->GetRDGMetrics().SetSink({});
    }
};

TEST_P(DelayedRadianceTest, PublishesPairedCachesAcrossDeclineResizeResetAndIdleUpdates)
{
    TestViewport viewport;

    HeapVector<RHIBuffer*> ownedBuffers;

    HeapVector<RHITexture*> ownedTextures;

    AllocateRendererInputs(0, viewport, ownedBuffers, ownedTextures);

    sceneInputs.camera.projViewMatrix = Mat4(1);

    sceneInputs.camera.proj           = Mat4(1);

    sceneInputs.camera.view           = Mat4(1);

    sg::Scene source;

    source.GetAABB() = sg::AABB(Vec3(-1), Vec3(1));

    SceneData data{};

    data.pScene = &source;

    RenderScene scene(device, data);

    TestVoxelVolumes volumes(device, DataFormat::eR8G8B8A8UNORM);

    volumes.Init();

    volumes.SetRenderScene(&scene);

    VoxelGIRenderer gi(device, &volumes);

    gi.SetRenderScene(&scene);

    ASSERT_TRUE(gi.Init());

    VoxelGISettings settings;

    settings.previousFrameRadiance = true;

    settings.bounceSource          = VoxelGISettings::BounceSource::Rays;

    DeferredLightingRenderer renderer(device);

    renderer.Init();

    renderer.SetRenderScene(&scene);

    RenderView view             = RenderView::FromViewport(viewport);

    RHITexture* primary         = gi.GetRadianceTexture();

    RHITexture* alternate       = nullptr;

    uint64_t generation         = 0;

    uint64_t consumedGeneration = 0;

    for (uint32_t frame = 0; frame < 18; ++frame)
    {
        SCOPED_TRACE(frame);

        const bool declined = frame == 0 || frame == 3;

        const bool failed   = frame == 8;

        const bool accepted = !declined && !failed;

        const bool update   = frame != 5 && frame != 10 && frame != 13 && frame != 17;

        const bool delayed  = update && ((frame >= 2 && frame <= 8) || frame == 14 || frame == 16);

        if (frame == 2 || frame == 3 || frame == 6 || frame == 7 || frame == 8 || frame == 11 || frame == 12
            || frame >= 14 && frame <= 16)
        {
            settings.analyticLighting = !settings.analyticLighting;
        }

        if (frame == 6)
        {
            view.width *= 2;
        }

        if (frame == 10)
        {
            settings.previousFrameRadiance = false;
        }

        if (frame == 12)
        {
            // A new scene bootstraps immediately even after the delayed cache has been valid.
            settings.previousFrameRadiance = true;

            gi.SetRenderScene(&scene);
        }

        if (frame == 14 || frame == 15)
        {
            settings.bounceSource = frame == 14 ? VoxelGISettings::BounceSource::Cone : VoxelGISettings::BounceSource::Rays;

            settings.reflections  = frame == 14 ? VoxelGISettings::Reflections::Off : VoxelGISettings::Reflections::On;
        }

        ASSERT_TRUE(gi.SetSettings(settings));

        RenderGraph& graph = *device->GetCurrentFrameRDG();

        ASSERT_TRUE(graph.Begin());

        if (frame <= 1)
        {
            for (RHITexture* texture : ownedTextures)
            {
                if (texture != viewport.depth)
                {
                    graph.AddTransferPass("InitializeDelayedInput").ClearTexture(texture, Color(0));
                }
            }
        }

        if (volumes.BeginVolumeUpdate(graph, RDGQueuePreference::ePreferAsyncCompute))
        {
            for (RHITexture* texture :
                 {volumes.GetVoxelTextures().pAlbedo, volumes.GetVoxelTextures().pNormal, volumes.GetVoxelTextures().pEmissive})
            {
                graph.AddTransferPass("InitializeDelayedVoxels")
                    .SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute)
                    .ClearTexture(texture, Color(0));
            }
        }

        renderer.BuildGBufferGraph(view, true);

        HybridGIRenderer* hybrid = renderer.GetHybridGI();

        if (frame == 7)
        {
            hybrid->InvalidateHistory("test_reset");
        }

        gi.BuildRayQueryGraph();

        if (frame == 15)
        {
            // The alternate full chain survives, but restoring ray-hit history needs a new snapshot.
            rhi->failTextureCreationAt = rhi->textureCreations + 1;
        }

        gi.BuildRenderGraph();

        rhi->failTextureCreationAt = 0;

        EXPECT_EQ(gi.GetRadianceTexture(), primary);

        const uint64_t readGeneration = generation + (update && !delayed ? 1 : 0);

        EXPECT_EQ(gi.GetRadianceGeneration(), readGeneration);

        RDGPassDescBase hitInputs;

        gi.BindHitRadianceInputs(hitInputs);

        // Radiance, its previous level, normals and the reflectance the light-visibility cache weights.
        ASSERT_EQ(hitInputs.textureViewStorage.size(), 4u);

        EXPECT_EQ(hitInputs.textureViewStorage[0]->GetTexture(), primary);

        RHITexture* hitPrevious = hitInputs.textureViewStorage[1]->GetTexture();

        if (frame <= 1 || frame == 10 || frame == 12)
        {
            alternate = hitPrevious;
        }

        ASSERT_NE(alternate, nullptr);

        EXPECT_EQ(alternate->GetNumMipmaps(), settings.previousFrameRadiance ? primary->GetNumMipmaps() : 1u);

        if (frame == 14)
        {
            EXPECT_EQ(hitPrevious, primary);
        }
        else if (delayed)
        {
            EXPECT_NE(hitPrevious, primary);

            EXPECT_NE(hitPrevious, alternate);

            EXPECT_EQ(hitPrevious->GetNumMipmaps(), 1u);
        }
        else
        {
            EXPECT_EQ(hitPrevious, alternate);
        }

        RDGPassDescBase lightingInputs;

        gi.BindLightingInputs(lightingInputs);

        EXPECT_EQ(lightingInputs.textureViewStorage.back()->GetTexture(), primary);

        ASSERT_TRUE(hybrid->BuildRenderGraph(view, gi));

        if (frame == 6 || frame == 7 || frame == 10)
        {
            EXPECT_FALSE(hybrid->IsHistoryValid());

            EXPECT_STREQ(hybrid->GetResetReason(), frame == 6 ? "resize" : frame == 7 ? "test_reset" : "settings");
        }

        ASSERT_TRUE(graph.End()) << graph.GetResult().message;

        if (accepted)
        {
            rhi->graphics.imageCopyTextures.clear();

            rhi->compute.imageCopyTextures.clear();

            const size_t valuesBefore = rhi->graphics.values.size();

            device->GetRDGMetrics().RequestCapture();

            ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;

            const TestContext& copyContext = GetParam() % 2 == 0 ? rhi->graphics : rhi->compute;

            if ((delayed && frame != 14) || frame == 11 || frame == 15)
            {
                const std::pair<RHITexture*, RHITexture*> expected(delayed ? alternate : primary, hitPrevious);

                EXPECT_NE(std::find(copyContext.imageCopyTextures.begin(), copyContext.imageCopyTextures.end(), expected),
                          copyContext.imageCopyTextures.end());
            }

            if (delayed && GetParam() % 2 != 0)
            {
                uint32_t injectionGroup = UINT32_MAX;

                uint32_t traceGroup     = UINT32_MAX;

                for (const RDGNodeMetrics& node : device->GetRDGMetrics().GetLastSnapshot().nodes)
                {
                    if (node.name == NameID("VoxelInjectRadiance"))
                    {
                        EXPECT_EQ(node.plannedQueue, RHICommandContextType::eAsyncCompute);

                        injectionGroup = node.submissionGroup;
                    }

                    if (node.name == NameID("HybridTrace"))
                    {
                        traceGroup = node.submissionGroup;
                    }
                }

                const HeapVector<RDGSubmissionGroup>& groups = graph.GetSchedule().groups;

                ASSERT_LT(injectionGroup, groups.size());

                ASSERT_LT(traceGroup, groups.size());

                HeapVector<uint8_t> ancestors(groups.size());

                std::fill(ancestors.begin(), ancestors.end(), 0);

                ancestors[traceGroup] = 1;

                for (size_t group = traceGroup + 1; group > 0; --group)
                {
                    if (ancestors[group - 1] != 0)
                    {
                        for (uint32_t predecessor : groups[group - 1].predecessors)
                        {
                            ancestors[predecessor] = 1;
                        }
                    }
                }

                EXPECT_EQ(ancestors[injectionGroup], 0u) << "Hybrid trace must not wait for the pending radiance update";
            }

            bool foundUniforms = false;

            for (size_t value = valuesBefore; value < rhi->graphics.values.size(); ++value)
            {
                if (rhi->graphics.values[value].size() == sizeof(HybridGIUniformData))
                {
                    HybridGIUniformData uniforms;

                    std::memcpy(&uniforms, rhi->graphics.values[value].data(), sizeof(uniforms));

                    EXPECT_EQ(uniforms.bounce.w, readGeneration == consumedGeneration + 1 ? 1u : 0u);

                    EXPECT_EQ(uniforms.reflection.w, uniforms.bounce.w);

                    foundUniforms = true;
                }
            }

            EXPECT_TRUE(foundUniforms);

            consumedGeneration = readGeneration;
        }

        const RenderFrameResult result = declined ? RenderFrameResult::eDeclined
                                       : failed   ? RenderFrameResult::eFailed
                                                  : RenderFrameResult::eSubmitted;

        renderer.OnRenderGraphExecuted(result);

        gi.OnRenderGraphExecuted(result);

        volumes.OnRenderGraphExecuted(result);

        if (accepted && update)
        {
            ++generation;

            if (delayed)
            {
                std::swap(primary, alternate);
            }
        }

        EXPECT_EQ(gi.GetRadianceGeneration(), generation);

        EXPECT_EQ(gi.GetRadianceTexture(), primary);
    }

    renderer.Destroy();

    gi.Destroy();

    volumes.Destroy();

    for (RHIBuffer* buffer : ownedBuffers)
    {
        device->DestroyBuffer(buffer);
    }

    for (RHITexture* texture : ownedTextures)
    {
        device->DestroyTexture(texture);
    }

    sceneInputs = {};
}

INSTANTIATE_TEST_SUITE_P(InlineThreadedOffAuto, DelayedRadianceTest, testing::Values(0u, 1u, 2u, 3u));
