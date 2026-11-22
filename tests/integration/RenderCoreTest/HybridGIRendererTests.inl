void CreateHybridTestShaders(RenderDevice* device)
{
    for (const std::array<const char*, 2>& shader :
         {std::array<const char*, 2>{"HybridTraceHardwareSP", "VoxelGI/hybrid_trace_hardware.comp.spv"},
          {"HybridTraceHardwareCountsSP", "VoxelGI/hybrid_trace_hardware_counts.comp.spv"},
          {"HybridTraceReSTIRHardwareSP", "VoxelGI/hybrid_trace_restir_hardware.comp.spv"},
          {"HybridTraceNoGuideFieldSP", "VoxelGI/hybrid_trace_noguide_field.comp.spv"},
          {"HybridTraceHardwareNoGuideSP", "VoxelGI/hybrid_trace_hardware_noguide.comp.spv"},
          {"HybridTraceReSTIRHardwareNoGuideSP", "VoxelGI/hybrid_trace_restir_hardware_noguide.comp.spv"},
          {"ReSTIRTemporalSP", "VoxelGI/restir_temporal.comp.spv"},
          {"ReSTIRSpatialDiffuseSP", "VoxelGI/restir_spatial_diffuse.comp.spv"},
          {"ReSTIRSpatialReflectionSP", "VoxelGI/restir_spatial_reflection.comp.spv"},
          {"LightOccludersCountsSP", "ShadowMapping/light_occluders_counts.comp.spv"},
          {"LightOccludersSP", "ShadowMapping/light_occluders.comp.spv"},
          {"HybridDownsampleSP", "VoxelGI/hybrid_downsample.comp.spv"},
          {"HybridDownsampleFootprintSP", "VoxelGI/hybrid_downsample.comp.spv"},
          {"HybridReconstructSP", "VoxelGI/hybrid_reconstruct.comp.spv"},
          {"GIDenoiseStabilizeSP", "VoxelGI/gi_denoise_stabilize.comp.spv"},
          {"GIDenoiseStabilizeNoSkySP", "VoxelGI/gi_denoise_stabilize.comp.spv"},
          {"GIDenoisePreblurSP", "VoxelGI/gi_denoise_preblur.comp.spv"},
          {"GIDenoisePreblurNoSkySP", "VoxelGI/gi_denoise_preblur.comp.spv"},
          {"GIDenoiseTemporalSP", "VoxelGI/gi_denoise_temporal.comp.spv"},
          {"GIDenoiseHistoryFixSP", "VoxelGI/gi_denoise_history_fix.comp.spv"},
          {"GIDenoiseAtrous0SP", "VoxelGI/gi_denoise_atrous.comp.spv"},
          {"GIDenoiseAtrous1SP", "VoxelGI/gi_denoise_atrous.comp.spv"},
          {"GIDenoiseAtrous2SP", "VoxelGI/gi_denoise_atrous.comp.spv"},
          {"GIDenoiseTemporalNoSkySP", "VoxelGI/gi_denoise_temporal.comp.spv"},
          {"GIDenoiseHistoryFixNoSkySP", "VoxelGI/gi_denoise_history_fix.comp.spv"},
          {"GIDenoiseAtrous0NoSkySP", "VoxelGI/gi_denoise_atrous.comp.spv"},
          {"GIDenoiseAtrous1NoSkySP", "VoxelGI/gi_denoise_atrous.comp.spv"},
          {"GIDenoiseAtrous2NoSkySP", "VoxelGI/gi_denoise_atrous.comp.spv"},
          {"HybridReconstructNoSkySP", "VoxelGI/hybrid_reconstruct.comp.spv"},
          {"ReSTIRLobeIntegralSP", "VoxelGI/restir_lobe_integral.comp.spv"},
          {"HybridTraceFieldSP", "VoxelGI/hybrid_trace_field.comp.spv"},
          {"HybridTraceCountsFieldSP", "VoxelGI/hybrid_trace_counts_field.comp.spv"},
          {"EnvironmentColumnsSP", "VoxelGI/environment_columns.comp.spv"},
          {"EnvironmentRowsSP", "VoxelGI/environment_rows.comp.spv"},
          {"EnvironmentTilesSP", "VoxelGI/environment_tiles.comp.spv"},
          {"HybridTemporalReferenceSP", "VoxelGI/hybrid_temporal_reference.comp.spv"},
          // Default refinements: bicubic history, data-driven anti-lag, young-receiver samples and anti-firefly.
          {"GIDenoiseTemporalAntiLagBicubicSP", "VoxelGI/gi_denoise_temporal_antilag_bicubic.comp.spv"},
          {"GIDenoiseTemporalAntiLagBicubicNoSkySP", "VoxelGI/gi_denoise_temporal_antilag_bicubic.comp.spv"},
          {"GIDenoiseHistoryFixAntiLagAntiFireflySP", "VoxelGI/gi_denoise_history_fix_antilag_antifirefly.comp.spv"},
          {"GIDenoiseHistoryFixAntiLagAntiFireflyNoSkySP", "VoxelGI/gi_denoise_history_fix_antilag_antifirefly.comp.spv"},
          {"HybridTraceFieldYoungSP", "VoxelGI/hybrid_trace_field_young.comp.spv"},
          {"HybridTraceNoGuideFieldYoungSP", "VoxelGI/hybrid_trace_noguide_field_young.comp.spv"},
          {"HybridTraceHardwareYoungSP", "VoxelGI/hybrid_trace_hardware_young.comp.spv"},
          {"HybridTraceHardwareNoGuideYoungSP", "VoxelGI/hybrid_trace_hardware_noguide_young.comp.spv"},
          {"HybridTraceReSTIRHardwareYoungSP", "VoxelGI/hybrid_trace_restir_hardware_young.comp.spv"},
          {"HybridTraceReSTIRHardwareNoGuideYoungSP", "VoxelGI/hybrid_trace_restir_hardware_noguide_young.comp.spv"},
          {"VoxelFilterAlbedoSP", "VoxelGI/filter_albedo.comp.spv"},
          {"VoxelFilterRadianceSP", "VoxelGI/filter_radiance.comp.spv"},
          {"VoxelInjectRadianceSP", "VoxelGI/inject_radiance.comp.spv"}})
    {
        RHIShaderCreateInfo info;
        info.stageFlags.SetFlag(RHIShaderStageFlagBits::eCompute);
        info.spirvFileName[ToUnderlying(RHIShaderStage::eCompute)] = shader[1];
        RegisterReflectedShader(shader[0], info);
        CreateTestShaderProgram(device, shader[0]);
    }
    RHIShaderCreateInfo receiverInfo;
    receiverInfo.stageFlags.SetFlags(RHIShaderStageFlagBits::eVertex, RHIShaderStageFlagBits::eFragment);
    receiverInfo.spirvFileName[ToUnderlying(RHIShaderStage::eVertex)]   = "SceneRenderer/receiver.vert.spv";
    receiverInfo.spirvFileName[ToUnderlying(RHIShaderStage::eFragment)] = "SceneRenderer/receiver.frag.spv";
    RegisterReflectedShader("HybridReceiverSP", receiverInfo);
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

TEST_F(RenderCoreTest, ReSTIRHistoriesFollowLightingRevisionsAndDeclinedModeChanges)
{
    rhi->info.rayQuery              = {true, true, true, true, true, 256, 1024, 1024, 1024};
    rhi->info.maxStorageBufferRange = UINT32_MAX;
    CreateHybridTestShaders(device);
    TestViewport            viewport;
    HeapVector<RHIBuffer*>  ownedBuffers;
    HeapVector<RHITexture*> ownedTextures;
    AllocateRendererInputs(0, viewport, ownedBuffers, ownedTextures);
    sceneInputs.camera.projViewMatrix = sceneInputs.camera.proj = sceneInputs.camera.view = Mat4(1);
    sg::Scene source;
    source.GetAABB() = sg::AABB(Vec3(-1), Vec3(1));
    SceneData data{};
    data.pScene = &source;
    RenderScene      scene(device, data);
    SceneLight       light;
    const LightId    lightId = scene.GetLights().Add(light);
    TestVoxelVolumes volumes(device, DataFormat::eR8G8B8A8UNORM);
    volumes.Init();
    volumes.SetRenderScene(&scene);
    VoxelGIRenderer gi(device, &volumes);
    gi.SetRenderScene(&scene);
    ASSERT_TRUE(gi.Init());
    VoxelGISettings settings;
    settings.rayProvider = VoxelGISettings::RayProvider::Hardware;
    settings.restirGI    = true;
    ASSERT_TRUE(gi.SetSettings(settings));
    DeferredLightingRenderer renderer(device);
    renderer.Init();
    renderer.SetRenderScene(&scene);
    const RenderView view = RenderView::FromViewport(viewport);
    for (uint32_t frame = 0; frame < 16; ++frame)
    {
        SCOPED_TRACE(frame);
        if (frame == 2)
        {
            light.position.x = 0.1f;
            ASSERT_TRUE(scene.GetLights().Update(lightId, light));
        }
        if (frame == 4)
        {
            ASSERT_TRUE(scene.CommitGeometryUpdates());
        }
        if (frame == 5 || frame == 6)
        {
            settings.restirGI = frame == 6;
            ASSERT_TRUE(gi.SetSettings(settings));
        }
        if (frame == 8 || frame == 10 || frame == 13)
        {
            settings.pixelResolutionDivisor = frame == 8 ? 2u : (frame == 10 ? 4u : 1u);
            ASSERT_TRUE(gi.SetSettings(settings));
        }
        if (frame == 14 || frame == 15)
        {
            settings.pixelResolutionDivisor = 4;
            settings.referenceSamples       = frame == 14 ? 256u : 0u;
            ASSERT_TRUE(gi.SetSettings(settings));
        }
        RenderGraph& graph = *device->GetCurrentFrameRDG();
        ASSERT_TRUE(graph.Begin());
        if (frame == 0)
        {
            for (RHITexture* texture : ownedTextures)
            {
                if (texture != viewport.depth)
                {
                    graph.AddTransferPass("InitializeReSTIRInput").ClearTexture(texture, Color(0));
                }
            }
        }
        renderer.BuildGBufferGraph(view, true);
        gi.BuildRayQueryGraph();
        gi.BuildRenderGraph();
        HybridGIRenderer* hybrid = renderer.GetHybridGI();
        ASSERT_TRUE(hybrid->BuildRenderGraph(view, gi));
        const uint32_t divisor = settings.referenceSamples != 0 ? 1u : settings.pixelResolutionDivisor;
        EXPECT_EQ(hybrid->GetGIExtent(), (glm::uvec2(view.width, view.height) + glm::uvec2(divisor - 1u)) / divisor);
        ASSERT_TRUE(graph.End()) << graph.GetResult().message;
        const bool declined = frame == 5 || frame == 10;
        if (!declined)
        {
            const size_t begin = rhi->graphics.values.size();
            ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
            bool found = false;
            for (size_t i = begin; i < rhi->graphics.values.size(); ++i)
            {
                if (rhi->graphics.valueBindings[i] == (TestDescriptorLocation{1, 0})
                    && rhi->graphics.values[i].size() == sizeof(HybridGIUniformData))
                {
                    HybridGIUniformData uniforms;
                    std::memcpy(&uniforms, rhi->graphics.values[i].data(), sizeof(uniforms));
                    const uint32_t valid = frame == 1 || frame == 3 || frame == 7 || frame == 9 || frame == 12 ? 1u : 0u;
                    EXPECT_EQ(uniforms.bounce.y, valid);
                    EXPECT_EQ(uniforms.reflection.y, valid);
                    EXPECT_EQ(uniforms.reservoir.x, frame == 14 ? 0u : 3u);
                    found = true;
                }
            }
            EXPECT_TRUE(found);
        }
        const RenderFrameResult result = declined ? RenderFrameResult::eDeclined : RenderFrameResult::eSubmitted;
        renderer.OnRenderGraphExecuted(result);
        gi.OnVisibilityGraphExecuted(result);
        gi.OnRenderGraphExecuted(result);
        volumes.OnRenderGraphExecuted(result);
        EXPECT_EQ(hybrid->IsHistoryValid(), !declined);
        device->NextFrame();
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
    // Exercise reflected-hit histories on the compute-only test device.
    bounceSettings.reflections            = VoxelGISettings::Reflections::On;
    bounceSettings.pixelResolutionDivisor = 1; // Native histories; reduced output histories are exercised below.
    ASSERT_TRUE(gi.SetSettings(bounceSettings));
    // Compute GI traces the scene's distance field, published before the first gathered frame.
    sceneInputs.vertices = Buffer(sizeof(asset::Vertex));
    ownedBuffers.push_back(sceneInputs.vertices);
    ASSERT_TRUE(PrepareComputeField(device, gi));
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
        if (frame == 13 || frame == 14)
        {
            VoxelGISettings changed  = gi.GetSettings();
            changed.reflections      = frame == 14 ? VoxelGISettings::Reflections::On : VoxelGISettings::Reflections::Off;
            changed.analyticLighting = frame == 14;
            ASSERT_TRUE(gi.SetSettings(changed));
        }
        if (frame == 15)
        {
            VoxelGISettings changed = gi.GetSettings();
            changed.historyFrames   = 16;
            ASSERT_TRUE(gi.SetSettings(changed));
        }
        if (frame == 16)
        {
            // Ordinary camera motion keeps temporal history, but same-pixel
            // geometry weights must not reuse it.
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
        gi.BuildRayQueryGraph();
        gi.BuildRenderGraph();
        EXPECT_TRUE(gi.HasPreviousRadiance());
        const bool historyBeforeBuild = hybrid->IsHistoryValid();

        ASSERT_TRUE(hybrid->BuildRenderGraph(view, gi));
        EXPECT_EQ(hybrid->WereRayCountsRecorded(), frame >= 15);
        // Toggling reflections (frames 13 and 14) changes the denoiser's signal layout and resets history.
        const bool settingsReset =
            frame == 2 || frame == 6 || frame == 13 || frame == 14 || frame == 15 || frame == 18 || frame == 20;
        EXPECT_EQ(hybrid->IsHistoryValid(), frame == 1 || frame == 5 || (frame >= 9 && !settingsReset));
        if (settingsReset)
        {
            EXPECT_STREQ(hybrid->GetResetReason(), "settings");
        }
        if (frame == 7)
        {
            EXPECT_STREQ(hybrid->GetResetReason(), "environment");
        }
        const uint32_t before = hybrid->GetFrame();
        ASSERT_TRUE(graph->End()) << graph->GetResult().message;
        const bool accepted = frame != 1 && !declined;
        if (accepted)
        {
            const size_t firstValue    = rhi->graphics.values.size();
            const size_t firstDispatch = rhi->graphics.dispatches.size();
            ASSERT_TRUE(device->ExecuteRenderGraph(*graph)) << graph->GetResult().message;
            uint32_t                 referenceDispatches = 0;
            uint32_t                 fullTraces          = 0;
            const TestDispatchExtent guideGroups =
                (TestDispatchExtent{(view.width + 15u) / 16u, (view.height + 15u) / 16u, 1u});
            for (size_t index = firstDispatch; index < rhi->graphics.dispatches.size(); ++index)
            {
                const std::pair<RHIShader*, TestDispatchExtent>& dispatch = rhi->graphics.dispatches[index];
                const std::string                                shader =
                    dispatch.first != nullptr
                        ? dispatch.first->GetCreateInfo().spirvFileName[ToUnderlying(RHIShaderStage::eCompute)]
                        : std::string();
                if (shader == "VoxelGI/hybrid_temporal_reference.comp.spv")
                {
                    EXPECT_EQ(dispatch.second, (TestDispatchExtent{(view.width + 7u) / 8u, (view.height + 7u) / 8u, 1u}));
                    ++referenceDispatches;
                }
                // The ordinary trace runs one group per guide tile.
                if (shader == "VoxelGI/hybrid_trace_field.comp.spv" || shader == "VoxelGI/hybrid_trace_counts_field.comp.spv"
                    || shader == "VoxelGI/hybrid_trace_field_young.comp.spv")
                {
                    EXPECT_EQ(dispatch.second, guideGroups);
                    ++fullTraces;
                }
                if (shader == "VoxelGI/hybrid_trace_noguide_field.comp.spv"
                    || shader == "VoxelGI/hybrid_trace_noguide_field_young.comp.spv")
                {
                    EXPECT_EQ(dispatch.second, (TestDispatchExtent{(view.width + 7u) / 8u, (view.height + 7u) / 8u, 1u}));
                    ++fullTraces;
                }
            }
            EXPECT_EQ(referenceDispatches, frame == 18 || frame == 19 ? 1u : 0u);
            EXPECT_EQ(fullTraces, 1u);
            bool           foundUniforms = false;
            uint32_t       reuseValues   = 0;
            const uint32_t unchangedReceivers =
                frame == 5 || (frame >= 9 && frame <= 12) || frame == 17 || frame == 19 || frame == 21 ? 1u : 0u;
            for (size_t value = firstValue; value < rhi->graphics.values.size(); ++value)
            {
                if (rhi->graphics.valueBindings[value] == (TestDescriptorLocation{1, 28}))
                {
                    ASSERT_EQ(rhi->graphics.values[value].size(), sizeof(glm::uvec4));
                    glm::uvec4 reuse;
                    std::memcpy(&reuse, rhi->graphics.values[value].data(), sizeof(reuse));
                    // Only young-receiver sampling reads it: off for ray counts (from frame 15) and the
                    // reference mean (frames 18 and 19).
                    EXPECT_EQ(reuse, glm::uvec4(0u, 0u, 0u, frame < 15 ? 4u : 0u));
                    ++reuseValues;
                }
                if (rhi->graphics.valueBindings[value] == (TestDescriptorLocation{1, 0})
                    && rhi->graphics.values[value].size() == sizeof(HybridGIUniformData))
                {
                    HybridGIUniformData uniforms;
                    std::memcpy(&uniforms, rhi->graphics.values[value].data(), sizeof(uniforms));
                    EXPECT_EQ(uniforms.sampling.w, hybrid->IsHistoryValid() ? 1u : 0u);
                    EXPECT_EQ(uniforms.rejection.w, frame == 18 || frame == 19 ? 1.0f : 0.0f);
                    EXPECT_EQ(uniforms.resolution.w, unchangedReceivers);
                    // Only steady history and a settings reset after a successful frame can reuse the guide.
                    // Startup, failed-frame recovery, a new view, resize, environment and scene resets cannot.
                    EXPECT_EQ(uniforms.provider.y, frame == 5 || frame == 6 || frame >= 9 ? 1u : 0u);
                    EXPECT_EQ(uniforms.provider.w, 1u);
                    EXPECT_EQ(uniforms.bounce.x, 1u);
                    EXPECT_EQ(uniforms.reflection.x, frame == 13 ? 0u : 1u);
                    if (frame >= 9)
                    {
                        // An injection change keeps bounce history; intensity and source changes reset it.
                        // None of them touches sky.
                        EXPECT_EQ(uniforms.sampling.w, settingsReset ? 0u : 1u);
                        EXPECT_EQ(uniforms.bounce.y, frame == 11 || settingsReset ? 0u : 1u);
                        EXPECT_EQ(uniforms.bounce.z, 1u);
                        // Only the reference mean measures the change between the caches, and no
                        // injection happens during it.
                        EXPECT_EQ(uniforms.bounce.w, 0u);
                        EXPECT_EQ(uniforms.reflection.y,
                                  frame == 11 || frame == 13 || frame == 14 || frame == 15 || frame == 18 || frame == 20 ? 0u
                                                                                                                         : 1u);
                        EXPECT_EQ(uniforms.reflection.w, 0u);
                    }
                    foundUniforms = true;
                }
            }
            EXPECT_TRUE(foundUniforms);
            // Only the trace binds it.
            EXPECT_EQ(reuseValues, 1u);
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
    // Reduced Compute output history must follow submissions, independently of the GI-grid
    // history. Cover the first declined recording, a declined steady frame, resize and reset.
    for (uint32_t divisor : {2u, 1u})
    {
        bounceSettings.pixelResolutionDivisor = divisor;
        ASSERT_TRUE(gi.SetSettings(bounceSettings));
        renderer.GetHybridGI()->SetRayCountCapture(nullptr, nullptr);
        const uint32_t expectedValid[] = {0, 0, 1, 1, 0, 1, 0, 0};
        for (uint32_t step = 0; step < 8; ++step)
        {
            SCOPED_TRACE(step);
            device->NextFrame();
            if (step == 4)
            {
                view.width += 16;
            }
            if (step == 6)
            {
                renderer.GetHybridGI()->InvalidateHistory("output_test_reset");
            }
            RenderGraph& graph = *device->GetCurrentFrameRDG();
            ASSERT_TRUE(graph.Begin());
            renderer.BuildGBufferGraph(view, true);
            gi.BuildRayQueryGraph();
            gi.BuildRenderGraph();
            ASSERT_TRUE(renderer.GetHybridGI()->BuildRenderGraph(view, gi));
            EXPECT_EQ(renderer.GetHybridGI()->GetGIExtent(), (glm::uvec2(view.width, view.height) + divisor - 1u) / divisor);
            ASSERT_TRUE(graph.End()) << graph.GetResult().message;
            const bool declined = step == 0 || step == 2 || step == 6;
            if (!declined)
            {
                const size_t firstValue    = rhi->graphics.values.size();
                const size_t firstDispatch = rhi->graphics.dispatches.size();
                ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
                uint32_t stabilized    = 0;
                uint32_t denoisePasses = 0;
                for (size_t index = firstDispatch; index < rhi->graphics.dispatches.size(); ++index)
                {
                    const std::pair<RHIShader*, TestDispatchExtent>& dispatch = rhi->graphics.dispatches[index];
                    const std::string                                shader =
                        dispatch.first != nullptr
                            ? dispatch.first->GetCreateInfo().spirvFileName[ToUnderlying(RHIShaderStage::eCompute)]
                            : std::string();
                    EXPECT_EQ(shader.find("VoxelGI/hybrid_temporal"), std::string::npos);
                    denoisePasses += shader.starts_with("VoxelGI/gi_denoise_") ? 1u : 0u;
                    if (dispatch.first != nullptr
                        && dispatch.first->GetCreateInfo().spirvFileName[ToUnderlying(RHIShaderStage::eCompute)]
                               == "VoxelGI/gi_denoise_stabilize.comp.spv")
                    {
                        EXPECT_EQ(dispatch.second, (TestDispatchExtent{(view.width + 7u) / 8u, (view.height + 7u) / 8u, 1u}));
                        ++stabilized;
                    }
                }
                EXPECT_EQ(stabilized, 1u);
                // Preblur, temporal, history fix, three a-trous iterations and stabilization.
                EXPECT_EQ(denoisePasses, 7u);
                bool                found = false;
                HybridGIUniformData outputUniforms;
                for (size_t index = firstValue; index < rhi->graphics.values.size(); ++index)
                {
                    if (rhi->graphics.valueBindings[index] == (TestDescriptorLocation{1, 0})
                        && rhi->graphics.values[index].size() == sizeof(HybridGIUniformData))
                    {
                        std::memcpy(&outputUniforms, rhi->graphics.values[index].data(), sizeof(outputUniforms));
                        found = true;
                    }
                }
                ASSERT_TRUE(found);
                // Stabilization is the last GI pass and binds its own validity in this uniform.
                EXPECT_EQ(outputUniforms.sampling.w, expectedValid[step]);
                EXPECT_EQ(outputUniforms.reservoir.w, 1u);
                EXPECT_EQ(outputUniforms.bounce.w, 0u);
                EXPECT_EQ(outputUniforms.reflection.w, 0u);
            }
            const RenderFrameResult result = declined ? RenderFrameResult::eDeclined : RenderFrameResult::eSubmitted;
            renderer.OnRenderGraphExecuted(result);
            gi.OnRenderGraphExecuted(result);
            volumes.OnRenderGraphExecuted(result);
        }
    }
    // Disabled sky lighting must not retain full-resolution slow/fast sky images.
    // Exercise enabling, disabling, and a declined recording through the real resource bindings.
    for (uint32_t step = 0; step < 5; ++step)
    {
        SCOPED_TRACE(step);
        device->NextFrame();
        const bool environment = step == 1 || step >= 3;
        const bool skyLight    = environment && step != 3;
        ASSERT_TRUE(scene.SetEnvironmentLighting(environment ? 1.0f : 0.0f, 0, true, false));
        bounceSettings.environmentLighting    = step != 3;
        bounceSettings.pixelResolutionDivisor = 2;
        ASSERT_TRUE(gi.SetSettings(bounceSettings));
        RenderGraph& graph = *device->GetCurrentFrameRDG();
        ASSERT_TRUE(graph.Begin());
        renderer.BuildGBufferGraph(view, true);
        gi.BuildRayQueryGraph();
        gi.BuildRenderGraph();
        ASSERT_TRUE(renderer.GetHybridGI()->BuildRenderGraph(view, gi));
        ASSERT_TRUE(graph.End()) << graph.GetResult().message;
        const bool declined = step == 2;
        if (!declined)
        {
            const size_t firstBinding = rhi->graphics.boundShaderResources.size();
            ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
            uint32_t         histories     = 0;
            uint32_t         intermediates = 0;
            const glm::uvec2 extent        = skyLight ? renderer.GetHybridGI()->GetGIExtent() : glm::uvec2(1);
            for (size_t index = firstBinding; index < rhi->graphics.boundShaderResources.size(); ++index)
            {
                const std::pair<RHIShader*, RHIShaderResourceParameter>& binding  = rhi->graphics.boundShaderResources[index];
                RHIResource*                                             resource = binding.second.pResource;
                if (resource != nullptr && resource->GetResourceType() == RHIResourceType::eTextureView)
                {
                    // Pooled allocation labels outlive their logical RDG resources. Identify the
                    // actual binding by shader and descriptor, including when another pass reuses it.
                    ASSERT_NE(binding.first, nullptr);
                    const RHIShaderResourceDescriptor* descriptor =
                        binding.first->GetSRDByLocation(binding.second.set, binding.second.binding);
                    ASSERT_NE(descriptor, nullptr);
                    const std::string_view name = descriptor->name.c_str();
                    const std::string      shader =
                        binding.first->GetCreateInfo().spirvFileName[ToUnderlying(RHIShaderStage::eCompute)];
                    RHITexture* texture = static_cast<RHITextureView*>(resource)->GetTexture();
                    const bool  history =
                        shader == "VoxelGI/gi_denoise_temporal_antilag_bicubic.comp.spv"
                        && (name == "previousSky" || name == "previousFastSky" || name == "nextSky" || name == "nextFastSky");
                    const bool intermediate = name == "outputSky"
                                           && (shader == "VoxelGI/gi_denoise_history_fix_antilag_antifirefly.comp.spv"
                                               || shader == "VoxelGI/gi_denoise_atrous.comp.spv");
                    if (history || intermediate)
                    {
                        EXPECT_EQ(glm::uvec2(texture->GetWidth(), texture->GetHeight()), extent) << shader << ':' << name;
                        histories     += history ? 1u : 0u;
                        intermediates += intermediate ? 1u : 0u;
                    }
                }
            }
            EXPECT_EQ(histories, 4u);
            EXPECT_EQ(intermediates, 4u);
        }
        const RenderFrameResult result = declined ? RenderFrameResult::eDeclined : RenderFrameResult::eSubmitted;
        renderer.OnRenderGraphExecuted(result);
        gi.OnRenderGraphExecuted(result);
        volumes.OnRenderGraphExecuted(result);
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

class RadianceHistoryTest : public RenderCoreTest, public testing::WithParamInterface<uint32_t>
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

TEST_P(RadianceHistoryTest, PublishesPairedCachesAcrossDeclineResizeResetAndIdleUpdates)
{
    TestViewport viewport;

    HeapVector<RHIBuffer*> ownedBuffers;

    HeapVector<RHITexture*> ownedTextures;

    AllocateRendererInputs(0, viewport, ownedBuffers, ownedTextures);

    // Field support before the first GI frame: a rejected field is not retried for the same scene.
    sceneInputs.vertices = Buffer(sizeof(asset::Vertex));
    ownedBuffers.push_back(sceneInputs.vertices);
    EnableDistanceFieldTestDevice(device);

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


    DeferredLightingRenderer renderer(device);

    renderer.Init();

    renderer.SetRenderScene(&scene);

    RenderView view       = RenderView::FromViewport(viewport);

    RHITexture* primary   = gi.GetRadianceTexture();

    RHITexture* alternate = nullptr;

    uint64_t generation   = 0;

    for (uint32_t frame = 0; frame < 18; ++frame)
    {
        SCOPED_TRACE(frame);

        const bool declined = frame == 0 || frame == 3;

        const bool failed   = frame == 8;

        const bool accepted = !declined && !failed;

        const bool update   = frame != 5 && frame != 10 && frame != 13 && frame != 17;

        if (frame == 2 || frame == 3 || frame == 6 || frame == 7 || frame == 8 || frame == 11 || frame == 12
            || frame >= 14 && frame <= 16)
        {
            settings.analyticLighting = !settings.analyticLighting;
        }

        if (frame == 6)
        {
            view.width *= 2;
        }

        if (frame == 12)
        {
            // A new scene invalidates the cached lighting before its next injection.

            gi.SetRenderScene(&scene);
        }

        if (frame == 14 || frame == 15)
        {

            settings.reflections = frame == 14 ? VoxelGISettings::Reflections::Off : VoxelGISettings::Reflections::On;
        }

        ASSERT_TRUE(gi.SetSettings(settings));

        sceneInputs.vertices = Buffer(sizeof(asset::Vertex));
        ownedBuffers.push_back(sceneInputs.vertices);
        ASSERT_TRUE(PrepareComputeField(device, gi));

        RenderGraph& graph = *device->GetCurrentFrameRDG();

        ASSERT_TRUE(graph.Begin());

        if (frame <= 1)
        {
            for (RHITexture* texture : ownedTextures)
            {
                if (texture != viewport.depth)
                {
                    graph.AddTransferPass("InitializeHistoryInput").ClearTexture(texture, Color(0));
                }
            }
        }

        if (volumes.BeginVolumeUpdate(graph, RDGQueuePreference::ePreferAsyncCompute))
        {
            for (RHITexture* texture :
                 {volumes.GetVoxelTextures().pAlbedo, volumes.GetVoxelTextures().pNormal, volumes.GetVoxelTextures().pEmissive})
            {
                graph.AddTransferPass("InitializeHistoryVoxels")
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

        gi.BuildRenderGraph();

        EXPECT_EQ(gi.GetRadianceTexture(), primary);

        const uint64_t readGeneration = generation + (update ? 1 : 0);

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

        EXPECT_EQ(alternate->GetNumMipmaps(), 1u);

        EXPECT_EQ(hitPrevious, alternate);

        ASSERT_TRUE(hybrid->BuildRenderGraph(view, gi));

        if (frame == 6 || frame == 7)
        {
            EXPECT_FALSE(hybrid->IsHistoryValid());

            EXPECT_STREQ(hybrid->GetResetReason(), frame == 6 ? "resize" : "test_reset");
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

            if (update && frame > 1 && frame != 9 && frame != 12)
            {
                const std::pair<RHITexture*, RHITexture*> expected(primary, hitPrevious);

                EXPECT_NE(std::find(copyContext.imageCopyTextures.begin(), copyContext.imageCopyTextures.end(), expected),
                          copyContext.imageCopyTextures.end());
            }

            bool foundUniforms = false;

            for (size_t value = valuesBefore; value < rhi->graphics.values.size(); ++value)
            {
                if (rhi->graphics.valueBindings[value] == (TestDescriptorLocation{1, 0})
                    && rhi->graphics.values[value].size() == sizeof(HybridGIUniformData))
                {
                    HybridGIUniformData uniforms;

                    std::memcpy(&uniforms, rhi->graphics.values[value].data(), sizeof(uniforms));

                    // Caches stay paired for the reference mean; the denoiser never asks the trace
                    // for the previous cache's radiance.
                    EXPECT_EQ(uniforms.bounce.w, 0u);

                    EXPECT_EQ(uniforms.reflection.w, uniforms.bounce.w);

                    foundUniforms = true;
                }
            }

            EXPECT_TRUE(foundUniforms);
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

INSTANTIATE_TEST_SUITE_P(InlineThreadedOffAuto, RadianceHistoryTest, testing::Values(0u, 1u, 2u, 3u));
