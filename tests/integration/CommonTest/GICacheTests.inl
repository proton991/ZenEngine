TEST_P(RayQueryIntegrationTest, RadianceCacheKeysCollisionsResolveAndEviction)
{
    for (uint32_t bits : {32u, 64u})
    {
        if (bits == 32u || device->GetGPUInfo().supportBufferInt64Atomics)
        {
            SCOPED_TRACE(bits);
            const NameID resolveName(fmt::format("CacheResolveCheck{}SP", bits));
            const NameID checkName(fmt::format("CacheCheck{}SP", bits));
            ShaderProgramManager::GetInstance().StoreProgram(
                ZEN_NEW() ComputeFileSP(device, resolveName, fmt::format("VoxelGI/gi_cache_resolve{}.comp.spv", bits).c_str()));
            ShaderProgramManager::GetInstance().StoreProgram(ZEN_NEW() ComputeFileSP(
                device, checkName, fmt::format("VoxelGI/Calibration/gi_cache_check{}.comp.spv", bits).c_str()));
            RHIFlags<RHIBufferUsageFlagBits> storage;
            storage.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer);
            RHIBuffer* cache[] = {Buffer(16 * 8, storage, nullptr, RHIBufferAllocateType::eGPU),
                                  Buffer(16 * 16, storage, nullptr, RHIBufferAllocateType::eGPU),
                                  Buffer(16 * 16, storage, nullptr, RHIBufferAllocateType::eGPU)};
            RHIBuffer* output  = Buffer(3 * sizeof(Vec4), storage, nullptr, RHIBufferAllocateType::eGPU);
            RHIBuffer* readback =
                Buffer(3 * sizeof(Vec4), RHIFlags<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferDstBuffer), nullptr,
                       RHIBufferAllocateType::eCPURead);
            GICacheUniformData uniform;
            uniform.limits      = glm::uvec4(16, 4, 2, 0);
            const char* names[] = {"GICacheKeys", "GICacheAccumulation", "GICacheResolved"};
            // Key tests, clear, 640 contending samples, resolve, lookup, two stale frames, lookup.
            // Splitting recordings also verifies imported-buffer dependencies across submissions.
            for (uint32_t frame = 0; frame < 3; ++frame)
            {
                RenderGraph& graph = *device->GetCurrentFrameRDG();
                ASSERT_TRUE(graph.Begin());
                const uint32_t operations[] = {3, 0, 3, 1, 4, 2};
                const uint32_t begin        = frame == 0 ? 0 : 4;
                for (uint32_t step = begin; step < 6; ++step)
                {
                    const uint32_t operation = operations[step];
                    uniform.grid.w           = operation == 3 ? 1.0f : 0.0f;
                    RDGComputePassDesc pass;
                    pass.SetShaderProgramName(operation >= 3 ? resolveName : checkName);
                    pass.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
                    pass.BindValue("uGICacheData", uniform);
                    for (uint32_t index = 0; index < 3; ++index)
                    {
                        pass.BindStorageBuffer(names[index], cache[index],
                                               operation == 3 ? RDGContentGuarantee::eFullWrite : RDGContentGuarantee::eNone);
                    }
                    if (operation < 3)
                    {
                        pass.BindStorageBuffer("Results", output,
                                               frame == 0 && operation == 0 ? RDGContentGuarantee::eFullWrite
                                                                            : RDGContentGuarantee::eNone);
                    }
                    graph.AddComputePass(std::move(pass)).RecordPassCommands([operation](RDGPassCmdEncoder& encoder) {
                        if (operation < 3)
                        {
                            encoder.SetPushConstants(operation);
                        }
                        encoder.Dispatch(operation == 1 ? 10 : 1, 1, 1);
                    });
                }
                graph.AddTransferPass("ReadCacheChecks").CopyBuffer(output, readback, {0, 0, 3 * sizeof(Vec4)});
                ASSERT_TRUE(graph.End()) << graph.GetResult().message;
                ASSERT_TRUE(device->ExecuteRenderGraph(graph));
                device->WaitForIdle();
                const Vec4* result = reinterpret_cast<const Vec4*>(readback->Map());
                EXPECT_EQ(result[0], Vec4(0, 2, 1, 1));
                EXPECT_EQ(result[1], Vec4(1, 1, 1, 0));
                EXPECT_EQ(result[2], frame < 2 ? Vec4(.125f, .25f, .5f, 1) : Vec4(0));
                readback->Unmap();
            }
        }
    }
}

TEST_P(RayQueryIntegrationTest, WorldCacheDirectionalSamplesInheritResolveAndExpire)
{
    for (uint32_t bits : {32u, 64u})
    {
        if (bits == 32u || device->GetGPUInfo().supportBufferInt64Atomics)
        {
            SCOPED_TRACE(bits);
            const NameID resolveName(fmt::format("WorldResolveCheck{}SP", bits));
            const NameID checkName(fmt::format("WorldCheck{}SP", bits));
            ShaderProgramManager::GetInstance().StoreProgram(ZEN_NEW() ComputeFileSP(
                device, resolveName, fmt::format("VoxelGI/world_cache_resolve{}.comp.spv", bits).c_str()));
            ShaderProgramManager::GetInstance().StoreProgram(ZEN_NEW() ComputeFileSP(
                device, checkName, fmt::format("VoxelGI/Calibration/world_cache_check{}.comp.spv", bits).c_str()));
            RHIFlags<RHIBufferUsageFlagBits> storage;
            storage.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer);
            const uint32_t       sizes[] = {16 * 8, 16 * 112, 16 * 112, 16 * 12, WorldCacheLightingBytes};
            const char*          names[] = {"GICacheKeys", "GICacheAccumulation", "GICacheResolved", "WorldCacheCells",
                                            "WorldCacheLighting"};
            HeapVector<uint32_t> zero(std::max(16u * 112u, WorldCacheLightingBytes) / sizeof(uint32_t), 0u);
            RHIBuffer*           cache[5]{};
            for (uint32_t index = 0; index < std::size(cache); ++index)
            {
                cache[index] = Buffer(sizes[index], storage, zero.data());
            }
            constexpr uint32_t bytes  = 5 * sizeof(Vec4);
            RHIBuffer*         output = Buffer(bytes, storage, zero.data());
            RHIBuffer*         readback =
                Buffer(2u * bytes, RHIFlags<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferDstBuffer), nullptr,
                       RHIBufferAllocateType::eCPURead);
            GICacheUniformData uniform;
            uniform.limits     = glm::uvec4(16, 128, 240, 0);
            uniform.grid       = Vec4(1, 1, 1, 0);
            RenderGraph& graph = *device->GetCurrentFrameRDG();
            ASSERT_TRUE(graph.Begin());
            for (uint32_t step = 0; step < 5; ++step)
            {
                const bool          check = step % 2u == 0u;
                WorldCacheFrameData world;
                world.lightCount.y = 7u;
                world.schedule.x   = step < 3 ? 1u : 241u;
                RDGComputePassDesc pass;
                pass.SetShaderProgramName(check ? checkName : resolveName);
                pass.BindValue("uGICacheData", uniform);
                pass.BindValue("uWorldCacheData", world);
                for (uint32_t index = 0; index < std::size(cache); ++index)
                {
                    pass.BindStorageBuffer(names[index], cache[index]);
                }
                if (check)
                {
                    pass.BindStorageBuffer("Results", output);
                }
                graph.AddComputePass(std::move(pass)).RecordPassCommands([step, check](RDGPassCmdEncoder& encoder) {
                    if (check)
                    {
                        encoder.SetPushConstants(step);
                    }
                    encoder.Dispatch(1, 1, 1);
                });
                if (check && step >= 2u)
                {
                    graph.AddTransferPass("ReadWorldCacheChecks")
                        .CopyBuffer(output, readback, {0, (step / 2u - 1u) * bytes, bytes});
                }
            }
            ASSERT_TRUE(graph.End()) << graph.GetResult().message;
            ASSERT_TRUE(device->ExecuteRenderGraph(graph));
            device->WaitForIdle();
            const Vec4* captured = reinterpret_cast<const Vec4*>(readback->Map());
            ASSERT_NE(captured, nullptr);
            EXPECT_NEAR(captured[0].x, 2.0f, .002f); // Normalized chroma uses FP16.
            EXPECT_EQ(Vec3(captured[0].y, captured[0].z, captured[0].w), Vec3(0, 1, 1));
            EXPECT_NEAR(captured[1].x, 2.0f * 15.0f / 16.0f, .02f);
            EXPECT_EQ(Vec3(captured[1].y, captured[1].z, captured[1].w), Vec3(1, 1, 0));
            EXPECT_EQ(captured[2], Vec4(.125f, 16, 2, 0));
            EXPECT_NEAR(captured[3].x, 0.0f, .002f);
            EXPECT_NEAR(captured[3].y, .5f * (1.0f + std::sqrt(.5f)), .002f);
            EXPECT_NEAR(captured[3].z, captured[3].y, .002f);
            EXPECT_NEAR(captured[3].w, 1.0f, .002f);
            EXPECT_NEAR(captured[4].x, .0625f, .001f);
            EXPECT_EQ(Vec3(captured[4].y, captured[4].z, captured[4].w), Vec3(1, 0, 0));
            EXPECT_EQ(captured[6], Vec4(0));
            EXPECT_EQ(captured[9], Vec4(0));
            readback->Unmap();
        }
    }
}

TEST_P(RayQueryIntegrationTest, RadianceCachePreservesCalibrationMeanAndResets)
{
    if (SupportsHybridRayTracingPipeline(device->GetGPUInfo()))
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
        ASSERT_TRUE(fixture.scene->UpdateMaterial(0, material));
        SceneLight light;
        light.type         = SceneLightType::eDirectional;
        light.direction    = Vec3(0, 0, 1);
        light.intensity    = 1;
        light.castsShadows = false;
        ASSERT_NE(fixture.scene->GetLights().Add(light), 0u);
        constexpr uint32_t side = 65, pixels = side * side;
        rc::TextureFormat  format;
        format.width = format.height = side;
        format.depth                 = 1;
        format.format                = DataFormat::eR8G8B8A8UNORM;
        RHITexture* color            = device->CreateTextureColorRT(format, {}, "CacheCalibrationColor");
        format.format                = DataFormat::eD32SFloat;
        RHITexture* depth            = device->CreateTextureDepthStencilRT(format, {}, "CacheCalibrationDepth");
        textures.push_back(color);
        textures.push_back(depth);
        RHIFlags<RHIBufferUsageFlagBits> storage;
        storage.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer);
        constexpr uint32_t bytes   = pixels * ZEN_HYBRID_CAPTURE_BYTES_PER_PIXEL;
        RHIBuffer*         capture = Buffer(bytes, storage, nullptr, RHIBufferAllocateType::eGPU);
        RHIBuffer*     readback    = Buffer(bytes, RHIFlags<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferDstBuffer),
                                            nullptr, RHIBufferAllocateType::eCPURead);
        const uint32_t countBytes  = uint32_t(HybridGIRenderer::GetRayCountCaptureBytes(side, side));
        RHIBuffer*     counts      = Buffer(countBytes, storage, nullptr, RHIBufferAllocateType::eGPU);
        RHIBuffer*     countReadback =
            Buffer(countBytes, RHIFlags<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferDstBuffer), nullptr,
                   RHIBufferAllocateType::eCPURead);
        DeferredLightingRenderer& renderer = *server.RequestDeferredLightingRenderer();
        renderer.SetHybridCapture(capture, readback);
        server.SetRenderOption(RenderOption::eVoxelGI);
        double   means[3]{};
        uint64_t cacheHits = 0;
        for (uint32_t mode = 0; mode < 3; ++mode)
        {
            VoxelGIRuntimeSettings settings      = server.GetVoxelGISettings();
            settings.resolution                  = 64;
            settings.cone.rayProvider            = VoxelGISettings::RayProvider::Hardware;
            settings.cone.rayTracingPipeline     = true;
            settings.cone.radianceCache          = mode == 1;
            settings.cone.nativeProbeBenchmark   = mode == 2;
            settings.cone.restirGI               = false;
            settings.cone.pixelResolutionDivisor = 1;
            ASSERT_TRUE(server.ApplyVoxelGISettings(settings));
            RenderView view{color, depth, side, side};
            view.historyId = 500 + mode;
            ASSERT_EQ(server.DispatchRenderWorkloads(view), RenderFrameResult::eSubmitted);
            device->WaitForPreviousFrames();
            device->NextFrame();
            renderer.GetHybridGI()->SetRayCountCapture(counts, countReadback);
            for (uint32_t frame = 0; frame < 32; ++frame)
            {
                SCOPED_TRACE(frame);
                ASSERT_EQ(server.DispatchRenderWorkloads(view), RenderFrameResult::eSubmitted);
                device->WaitForPreviousFrames();
                device->NextFrame();
                ASSERT_TRUE(renderer.GetHybridGI()->WasCaptureRecorded());
                const Vec4* values = reinterpret_cast<const Vec4*>(readback->Map());
                for (uint32_t pixel = 0; pixel < pixels; ++pixel)
                {
                    const Vec4 value  = values[pixel * ZEN_HYBRID_CAPTURE_COMPONENTS + 2];
                    means[mode]      += value.x + value.y + value.z;
                }
                readback->Unmap();
                const glm::uvec4* words  = reinterpret_cast<const glm::uvec4*>(countReadback->Map());
                const uint32_t    offset = ((side + 15) / 16) * ((side + 15) / 16) * 4;
                for (uint32_t pixel = 0; pixel < pixels; ++pixel)
                {
                    const glm::uvec4 counters = words[offset + pixel * 2 + 1];
                    EXPECT_LE(counters.y, counters.x);
                    cacheHits += mode == 1 ? counters.y : 0;
                }
                countReadback->Unmap();
                if (frame == 15)
                {
                    renderer.GetHybridGI()->InvalidateHistory("cache_reset_regression");
                }
            }
            EXPECT_EQ(renderer.GetHybridGI()->GetRadianceCacheCapacity() != 0, mode != 0);
            EXPECT_EQ(renderer.GetHybridGI()->GetNativeProbeRaySlots(), mode == 2 ? 2048u : 0u);
            renderer.GetHybridGI()->SetRayCountCapture(nullptr, nullptr);
        }
        EXPECT_GT(means[0], 0.0);
        EXPECT_GT(cacheHits, 0u);
        // Directional, unshadowed hit lighting is constant; the only cache error is FP16 storage.
        EXPECT_NEAR(means[1], means[0], means[0] * .001);
        // The diagnostic dispatch uses the cache, but does not select cache shading for pixel GI.
        EXPECT_NEAR(means[2], means[0], means[0] * .001);
        renderer.SetHybridCapture(nullptr, nullptr);
    }
}

TEST_P(RayQueryIntegrationTest, NativeProbesRecoverLightingWithoutCPURevisionHint)
{
    if (SupportsHybridRayTracingPipeline(device->GetGPUInfo()))
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
        ASSERT_TRUE(fixture.scene->UpdateMaterial(0, material));
        SceneLight light;
        light.type         = SceneLightType::eDirectional;
        light.direction    = Vec3(0, 0, 1);
        light.intensity    = 1;
        light.castsShadows = false;
        const LightId id   = fixture.scene->GetLights().Add(light);
        ASSERT_NE(id, 0u);
        const uint64_t revision = fixture.scene->GetLightingRevision();
        // With one path per 8x8 tile, this view gives the illumination response
        // fixture 289 paths per frame. Keep the odd extent to exercise partial tiles.
        constexpr uint32_t side = 129, pixels = side * side;
        rc::TextureFormat  format;
        format.width = format.height = side;
        format.depth                 = 1;
        format.format                = DataFormat::eR8G8B8A8UNORM;
        RHITexture* color            = device->CreateTextureColorRT(format, {}, "NativeResponseColor");
        format.format                = DataFormat::eD32SFloat;
        RHITexture* depth            = device->CreateTextureDepthStencilRT(format, {}, "NativeResponseDepth");
        textures.push_back(color);
        textures.push_back(depth);
        ASSERT_NE(color, nullptr);
        ASSERT_NE(depth, nullptr);
        RHIFlags<RHIBufferUsageFlagBits> storage;
        storage.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer);
        constexpr uint32_t bytes   = pixels * ZEN_HYBRID_CAPTURE_BYTES_PER_PIXEL;
        RHIBuffer*         capture = Buffer(bytes, storage, nullptr, RHIBufferAllocateType::eGPU);
        RHIBuffer* readback        = Buffer(bytes, RHIFlags<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferDstBuffer),
                                            nullptr, RHIBufferAllocateType::eCPURead);
        DeferredLightingRenderer& renderer = *server.RequestDeferredLightingRenderer();
        renderer.SetHybridCapture(capture, readback);
        server.SetRenderOption(RenderOption::eVoxelGI);
        VoxelGIRuntimeSettings settings  = server.GetVoxelGISettings();
        settings.resolution              = 64;
        settings.cone.rayProvider        = VoxelGISettings::RayProvider::Hardware;
        settings.cone.rayTracingPipeline = true;
        settings.cone.screenProbeGI      = true;
        settings.cone.worldCacheFeedback = true;
        settings.cone.nativeShadowReuse  = true;
        ASSERT_TRUE(server.ApplyVoxelGISettings(settings));
        const RenderView view{color, depth, side, side};
        double           means[7]{};
        double           cacheMeans[7]{};
        double           settledCacheMeans[7]{};
        uint32_t         cacheReceivers[7]{};
        uint32_t         cacheSupported[7]{};
        for (uint32_t phase = 0; phase < 7; ++phase)
        {
            // Deliberately bypass SceneLights::Update only in this fixture. The GPU sees a
            // changed light, while the renderer receives no revision/reset notification.
            SceneLight* edited = const_cast<SceneLight*>(fixture.scene->GetLights().Find(id));
            ASSERT_NE(edited, nullptr);
            edited->intensity = phase == 1 ? 0.0f : 1.0f;
            if (phase >= 5)
            {
                settings.cone.analyticLighting = phase != 5;
                ASSERT_TRUE(server.ApplyVoxelGISettings(settings));
            }
            for (uint32_t frame = 0; frame < 160; ++frame)
            {
                if ((phase == 3 || phase == 4) && frame < 64)
                {
                    const float step   = float(frame + 1u) / 64.0f;
                    const Vec3  eye    = phase == 4 ? Vec3(0, 0, -.1f * step) : Vec3(0);
                    const Vec3  target = eye + Vec3(.08f * (phase == 3 ? step : 1.0f), 0, -1);
                    fixture.camera.SetPose(eye, target);
                }
                ASSERT_EQ(server.DispatchRenderWorkloads(view), RenderFrameResult::eSubmitted);
                device->WaitForPreviousFrames();
                device->NextFrame();
                ASSERT_TRUE(renderer.GetHybridGI()->WasCaptureRecorded());
                const uint32_t cacheStart = phase == 3 || phase == 4 ? 128u : 64u;
                if (frame >= cacheStart && frame < cacheStart + 16u)
                {
                    const Vec4* values = reinterpret_cast<const Vec4*>(readback->Map());
                    ASSERT_NE(values, nullptr);
                    for (uint32_t pixel = 0; pixel < pixels; ++pixel)
                    {
                        const Vec4 value = values[pixel * ZEN_HYBRID_CAPTURE_COMPONENTS + 15];
                        EXPECT_TRUE(std::isfinite(value.x + value.y + value.z));
                        cacheMeans[phase]     += value.x + value.y + value.z;
                        const bool valid       = values[pixel * ZEN_HYBRID_CAPTURE_COMPONENTS + 8].w > 0;
                        cacheReceivers[phase] += uint32_t(valid);
                        cacheSupported[phase] += uint32_t(valid && value.w > 0);
                    }
                    readback->Unmap();
                }
                if (frame >= 144)
                {
                    const Vec4* values = reinterpret_cast<const Vec4*>(readback->Map());
                    ASSERT_NE(values, nullptr);
                    for (uint32_t pixel = 0; pixel < pixels; ++pixel)
                    {
                        const Vec4 value = values[pixel * ZEN_HYBRID_CAPTURE_COMPONENTS + 3];
                        EXPECT_TRUE(std::isfinite(value.x + value.y + value.z));
                        means[phase]             += value.x + value.y + value.z;
                        const Vec4 cacheValue     = values[pixel * ZEN_HYBRID_CAPTURE_COMPONENTS + 15];
                        settledCacheMeans[phase] += cacheValue.x + cacheValue.y + cacheValue.z;
                    }
                    readback->Unmap();
                }
            }
            EXPECT_EQ(fixture.scene->GetLightingRevision(), revision);
            EXPECT_NE(renderer.GetHybridGI()->GetScreenProbeCount(), 0u);
            EXPECT_GT(renderer.GetHybridGI()->GetFrame(), phase * 160u);
        }
        EXPECT_GT(means[0], 0.0);
        EXPECT_LT(means[1], means[0] * .05);
        EXPECT_NEAR(means[2], means[0], means[0] * .15);
        // Phase 1 response is measured directly in the cache, before probe filtering.
        // The cache must follow an unannounced on/off/on change within 80 frames.
        EXPECT_GT(cacheMeans[0], 0.0);
        EXPECT_LT(cacheMeans[1], cacheMeans[0] * .05);
        EXPECT_NEAR(cacheMeans[2], cacheMeans[0], cacheMeans[0] * .05);
        EXPECT_LT(cacheMeans[5], cacheMeans[4] * .05);
        EXPECT_NEAR(cacheMeans[6], cacheMeans[4], cacheMeans[4] * .05);
        for (uint32_t phase : {0u, 2u, 3u, 4u, 6u})
        {
            SCOPED_TRACE(phase);
            ASSERT_GT(cacheReceivers[phase], 0u);
            EXPECT_GE(double(cacheSupported[phase]) / cacheReceivers[phase], .99);
            // Compare each camera pose against its own later cache samples. These
            // receivers are lit indirectly, so their energy changes with the view.
            EXPECT_NEAR(cacheMeans[phase], settledCacheMeans[phase], settledCacheMeans[phase] * .05);
        }
        fixture.camera.SetPose(Vec3(.5f, 0, 0), Vec3(.5f, 0, -1));
        ASSERT_EQ(server.DispatchRenderWorkloads(view), RenderFrameResult::eSubmitted);
        device->WaitForPreviousFrames();
        device->NextFrame();
        EXPECT_STREQ(renderer.GetHybridGI()->GetResetReason(), "camera_cut_or_projection");
        ASSERT_EQ(server.DispatchRenderWorkloads(view), RenderFrameResult::eSubmitted);
        device->WaitForPreviousFrames();
        device->NextFrame();
        EXPECT_STREQ(renderer.GetHybridGI()->GetResetReason(), "none");
        // Returning after a camera cut resets screen history but must recover the
        // world's accumulated lighting. A cleared cell has at most eight samples
        // after this first frame; an older count proves that transport survived.
        fixture.camera.SetPose(Vec3(0, 0, -.1f), Vec3(.08f, 0, -1.1f));
        ASSERT_EQ(server.DispatchRenderWorkloads(view), RenderFrameResult::eSubmitted);
        device->WaitForPreviousFrames();
        device->NextFrame();
        EXPECT_STREQ(renderer.GetHybridGI()->GetResetReason(), "camera_cut_or_projection");
        const Vec4* recovered = reinterpret_cast<const Vec4*>(readback->Map());
        ASSERT_NE(recovered, nullptr);
        uint32_t retainedCacheReceivers = 0;
        for (uint32_t pixel = 0; pixel < pixels; ++pixel)
        {
            const uint32_t offset   = pixel * ZEN_HYBRID_CAPTURE_COMPONENTS;
            retainedCacheReceivers += uint32_t(recovered[offset + 8u].w > 0.0f && recovered[offset + 16u].w > 16.0f);
        }
        readback->Unmap();
        EXPECT_GT(retainedCacheReceivers, 0u);
        renderer.SetHybridCapture(nullptr, nullptr);
    }
}

TEST_P(RayQueryIntegrationTest, NativeComputeKeepsDynamicReceiversLitDuringFieldRebuilds)
{
    ShaderProgramManager::GetInstance().BuildShaderPrograms(device);
    device->InitializeRendererServer();
    RendererServer&          server = *device->GetRendererServer();
    ForwardBlendSceneFixture fixture(*device);
    ASSERT_TRUE(fixture.Init(1, false, 2, true));
    ASSERT_TRUE(fixture.scene->SetVoxelBounds(sg::AABB(Vec3(-2, -2, -5), Vec3(2, 2, 0))));
    ASSERT_TRUE(fixture.scene->SetEnvironmentLighting(.5f, 0, true, false));
    for (uint32_t instance = 0; instance < 3; ++instance)
    {
        ASSERT_TRUE(fixture.scene->SetInstanceClass(instance, GI_DYNAMIC));
    }
    constexpr uint32_t side = 65, pixels = side * side;
    rc::TextureFormat  format;
    format.width = format.height = side;
    format.depth                 = 1;
    format.format                = DataFormat::eR8G8B8A8UNORM;
    RHITexture* color            = device->CreateTextureColorRT(format, {}, "NativeDynamicColor");
    format.format                = DataFormat::eD32SFloat;
    RHITexture* depth            = device->CreateTextureDepthStencilRT(format, {}, "NativeDynamicDepth");
    textures.push_back(color);
    textures.push_back(depth);
    ASSERT_NE(color, nullptr);
    ASSERT_NE(depth, nullptr);
    RHIFlags<RHIBufferUsageFlagBits> storage;
    storage.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer);
    constexpr uint32_t bytes   = pixels * ZEN_HYBRID_CAPTURE_BYTES_PER_PIXEL;
    RHIBuffer*         capture = Buffer(bytes, storage, nullptr, RHIBufferAllocateType::eGPU);
    RHIBuffer* readback = Buffer(bytes, RHIFlags<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferDstBuffer), nullptr,
                                 RHIBufferAllocateType::eCPURead);
    ASSERT_NE(capture, nullptr);
    ASSERT_NE(readback, nullptr);
    DeferredLightingRenderer& renderer = *server.RequestDeferredLightingRenderer();
    renderer.SetHybridCapture(capture, readback);
    server.SetRenderOption(RenderOption::eVoxelGI);
    VoxelGIRuntimeSettings settings = server.GetVoxelGISettings();
    settings.resolution             = 64;
    settings.cone.rayProvider       = VoxelGISettings::RayProvider::Voxel;
    settings.cone.screenProbeGI     = true;
    ASSERT_TRUE(server.ApplyVoxelGISettings(settings));
    const RenderView                view{color, depth, side, side};
    double                          energy[2]{};
    const HeapVector<asset::Vertex> original = fixture.vertices;
    for (uint32_t phase = 0; phase < 2; ++phase)
    {
        for (uint32_t frame = 0; frame < 160; ++frame)
        {
            if (phase != 0)
            {
                for (size_t vertex = 0; vertex < original.size(); ++vertex)
                {
                    fixture.vertices[vertex].pos.z = original[vertex].pos.z + (frame % 2u == 0 ? .005f : -.005f);
                }
                ASSERT_TRUE(fixture.scene->UpdateVertices(0, MakeVecView(fixture.vertices)));
            }
            ASSERT_EQ(server.DispatchRenderWorkloads(view), RenderFrameResult::eSubmitted);
            device->WaitForPreviousFrames();
            device->NextFrame();
            if (frame >= 144)
            {
                ASSERT_TRUE(renderer.GetHybridGI()->WasCaptureRecorded());
                const Vec4* values = reinterpret_cast<const Vec4*>(readback->Map());
                ASSERT_NE(values, nullptr);
                for (uint32_t pixel = 0; pixel < pixels; ++pixel)
                {
                    const Vec4 sky  = values[pixel * ZEN_HYBRID_CAPTURE_COMPONENTS + 1u];
                    energy[phase]  += sky.x + sky.y + sky.z;
                }
                readback->Unmap();
            }
        }
    }
    EXPECT_GT(energy[0], 0.0);
    EXPECT_GT(energy[1], energy[0] * .5);
    EXPECT_LT(energy[1], energy[0] * 1.5);
    renderer.SetHybridCapture(nullptr, nullptr);
}
