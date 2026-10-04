TEST_F(RenderCoreTest, ShadowResolutionPreflightAccountsForRetiredMapsWithoutMutatingResources)
{
    constexpr uint64_t MiB = 1024ull * 1024;

    sg::Scene source;

    source.GetAABB() = sg::AABB(Vec3(-1), Vec3(1));

    SceneData data{};

    data.pScene = &source;

    RenderScene scene(device, data);

    sceneInputs.uniforms.lightInfo.x = MaxSceneLights;

    for (uint32_t index = 0; index < MaxSceneLights; ++index)
    {
        sceneInputs.uniforms.lights[index] = {Vec4(0, 0, 0, 4), Vec4(0, -1, 0, 1), Vec4(1), Vec4(0, 0, 1, 0)};
    }

    SceneShadowRenderer shadows(device);

    ASSERT_TRUE(shadows.SetResolution(2048));

    ASSERT_TRUE(shadows.Prepare(scene, true));

    const uint32_t allocations        = rhi->textureCreations;

    const uint32_t waits              = rhi->deviceIdleWaits;

    const size_t submissionWaits      = rhi->submissionWaits.size();

    const uint64_t mapsId             = rhi->createdTextureIds.back();

    rhi->info.deviceLocalMemoryBytes  = 4096 * MiB;

    rhi->memoryStats.deviceLocalBytes = 3584 * MiB;

    // Only 512 MiB is free, but Apply retires the 3072 MiB array and 16 MiB depth image.
    // Their 1024 replacements need 768 + 4 MiB.
    EXPECT_TRUE(shadows.Preflight(1024, 192));

    rhi->memoryStats.budgetAvailable = true;

    rhi->memoryStats.heapCount       = 1;

    rhi->memoryStats.heaps[0]        = {4096 * MiB, 3584 * MiB, 4096 * MiB, true};

    EXPECT_TRUE(shadows.Preflight(1024, 192));

    EXPECT_TRUE(shadows.Preflight(2048, 192));

    // A face-count change during a frame must allow the old and new arrays to coexist.
    EXPECT_FALSE(shadows.Preflight(2048, 198));

    EXPECT_FALSE(shadows.Preflight(4096, 192));

    EXPECT_FALSE(shadows.Preflight(1024, MaxSceneShadowFaces + 1));

    // Freeing the old maps does not help if unrelated usage still consumes the capacity/budget.
    rhi->memoryStats.deviceLocalBytes = (4096 + 3088) * MiB;

    EXPECT_FALSE(shadows.Preflight(1024, 192));

    rhi->memoryStats.deviceLocalBytes    = 3584 * MiB;

    rhi->memoryStats.heaps[0].usageBytes = (4096 + 3088) * MiB;

    EXPECT_FALSE(shadows.Preflight(1024, 192));

    EXPECT_EQ(rhi->textureCreations, allocations);

    EXPECT_EQ(rhi->deviceIdleWaits, waits);

    EXPECT_EQ(rhi->submissionWaits.size(), submissionWaits);

    EXPECT_EQ(destroyed.count(mapsId), 0u);

    shadows.Destroy();

    rhi->memoryStats.heaps[0].usageBytes = 3584 * MiB;

    // A renderer without live maps has no memory to reclaim, even if retirement is pending.
    EXPECT_FALSE(shadows.Preflight(1024, 192));

    rhi->memoryStats.deviceLocalBytes    = 0;

    rhi->memoryStats.heaps[0].usageBytes = 0;

    ASSERT_TRUE(shadows.SetResolution(1024));

    ASSERT_TRUE(shadows.Prepare(scene, true));

    rhi->memoryStats.deviceLocalBytes    = 3584 * MiB;

    rhi->memoryStats.heaps[0].usageBytes = 3584 * MiB;

    // Reclaiming smaller maps must not admit a replacement that still exceeds the budget.
    EXPECT_FALSE(shadows.Preflight(2048, 192));

    shadows.Destroy();
}

TEST_F(RenderCoreTest, SceneShadowsRetryFailedAllocationsAndExcludeDisabledLights)
{
    sg::Scene source;
    source.GetAABB() = sg::AABB(Vec3(-1), Vec3(1));
    SceneData data{};
    data.pScene = &source;
    RenderScene scene(device, data);
    sceneInputs.uniforms.lightInfo.x = 1;
    sceneInputs.uniforms.lights[0]   = {Vec4(0, 0, 0, 4), Vec4(0, -1, 0, 1), Vec4(1), Vec4(0, 0, 1, 0)};
    for (uint32_t failure = 1; failure <= 2; ++failure)
    {
        SceneShadowRenderer shadows(device);
        rhi->failTextureCreationAt = rhi->textureCreations + failure;
        EXPECT_FALSE(shadows.Prepare(scene, true));
        rhi->failTextureCreationAt = 0;
        ASSERT_TRUE(shadows.Prepare(scene, true));
        RDGComputePassDesc lighting;
        shadows.BindLightingInputs(lighting);
        SceneShadowUniformData uniforms;
        ASSERT_EQ(lighting.valueByteStorage.size(), sizeof(uniforms));
        std::memcpy(&uniforms, lighting.valueByteStorage.data(), sizeof(uniforms));
        EXPECT_EQ(uniforms.lights[0].y, 6.0f);
        ASSERT_TRUE(shadows.Prepare(scene, false));
        shadows.BindLightingInputs(lighting);
        std::memcpy(&uniforms, lighting.valueByteStorage.data(), sizeof(uniforms));
        EXPECT_EQ(uniforms.lights[0].y, 0.0f);
        shadows.Destroy();
    }
}

TEST_F(RenderCoreTest, SceneShadowsCacheStaticFacesAndInvalidateAfterLightGeometryOrHandoffChanges)
{
    RHIShaderCreateInfo shader;
    shader.stageFlags.SetFlags(RHIShaderStageFlagBits::eVertex, RHIShaderStageFlagBits::eFragment);
    shader.spirvFileName[ToUnderlying(RHIShaderStage::eVertex)]   = "ShadowMapping/scene_shadow.vert.spv";
    shader.spirvFileName[ToUnderlying(RHIShaderStage::eFragment)] = "ShadowMapping/scene_shadow.frag.spv";
    reflectedShaderInfos["SceneShadowSP"]                         = shader;
    CreateTestShaderProgram(device, "SceneShadowSP");
    CreateTestShaderProgram(device, "intent");
    sceneInputs.vertices  = Buffer(128);
    sceneInputs.indices   = Buffer();
    sceneInputs.nodes     = Buffer(128);
    sceneInputs.materials = Buffer(96);
    sceneInputs.uv        = Buffer();
    sceneInputs.textures.push_back(Texture());
    sg::Scene source;
    source.GetAABB() = sg::AABB(Vec3(-1), Vec3(1));
    SceneData data{};
    data.pScene = &source;
    RenderScene scene(device, data);
    sceneInputs.uniforms.lightInfo.x = 2;
    for (uint32_t index = 0; index < 2; ++index)
    {
        sceneInputs.uniforms.lights[index] = {Vec4(0, 0, 0, 4), Vec4(0, -1, 0, 1), Vec4(1), Vec4(0.9f, 0.8f, 1, 0)};
    }
    SceneShadowRenderer shadows(device);
    RenderGraph*        graph   = device->GetCurrentFrameRDG();
    RDGMetrics&         metrics = device->GetRDGMetrics();
    metrics.SetSink({});
    const uint32_t expectedFaces[] = {12, 0, 6, 0, 12, 12, 7, 6, 2, 6, 0, 0};
    for (uint32_t frame = 0; frame < std::size(expectedFaces); ++frame)
    {
        SCOPED_TRACE(frame);
        switch (frame)
        {
            case 2: sceneInputs.uniforms.lights[1].positionRange.x = 0.25f; break;
            case 3: sceneInputs.uniforms.lights[1].colorIntensity.x = 0.5f; break;
            case 6: sceneInputs.uniforms.lights[1].directionType.w = 2; break;
            case 7: sceneInputs.uniforms.lightInfo.x = 1; break;
            // GI caches visibility while lights are black; toggling intensity
            // neither loses their maps nor triggers redundant shadow rendering.
            case 10: sceneInputs.uniforms.lights[0].colorIntensity.w = 0; break;
            case 11: sceneInputs.uniforms.lights[0].colorIntensity.w = 2; break;
            default: break;
        }
        ASSERT_TRUE(shadows.Prepare(scene, frame != 8, frame >= 10));
        ASSERT_TRUE(graph->Begin());
        for (RHIBuffer* buffer : {sceneInputs.vertices, sceneInputs.indices, sceneInputs.nodes, sceneInputs.materials})
        {
            graph->GetResourceManager()->ImportHostWrittenBuffer(buffer);
        }
        if (frame == 0)
        {
            graph->AddTransferPass("InitializeMaterial").ClearTexture(sceneInputs.textures[0], Color(1));
        }
        shadows.BuildRenderGraph(scene, frame >= 5 ? 2 : 1);
        graph->AddComputePass(IntentPass("keepalive"));
        metrics.RequestCapture();
        ASSERT_TRUE(graph->End()) << graph->GetResult().message;
        ASSERT_TRUE(device->ExecuteRenderGraph(*graph)) << graph->GetResult().message;
        shadows.OnRenderGraphExecuted(frame != 3);
        EXPECT_EQ(CountGIPasses(metrics.GetLastSnapshot(), "SceneShadowFace_"), expectedFaces[frame]);
    }
    shadows.Destroy();
    for (RHIBuffer* buffer :
         {sceneInputs.vertices, sceneInputs.indices, sceneInputs.nodes, sceneInputs.materials, sceneInputs.uv})
    {
        device->DestroyBuffer(buffer);
    }
    device->DestroyTexture(sceneInputs.textures[0]);
}

TEST_F(RenderCoreTest, SceneSamplerBindingsPreserveAuthoredHeapSlots)
{
    CreateTestShaderProgram(device, "scene_sampler_slots");
    RHISampler* nearest = device->CreateSampler({});
    RHISampler* linear  = device->CreateSampler(RHISamplerCreateInfo::CreateLinearRepeat());
    RenderGraph graph("scene_sampler_slots");
    graph.Begin();

    RDGComputePassDesc pass;
    pass.SetShaderProgramName("scene_sampler_slots");
    pass.BindSampler("uSamplerHeap", nearest, 0);
    pass.BindSampler("uSamplerHeap", linear, 3);
    graph.AddComputePass(std::move(pass));
    ASSERT_TRUE(graph.End());

    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    ASSERT_EQ(rhi->graphics.boundArrayIndices.size(), 2u);
    EXPECT_EQ(rhi->graphics.boundArrayIndices[0], 0u);
    EXPECT_EQ(rhi->graphics.boundArrayIndices[1], 3u);
}

TEST_F(RenderCoreTest, SceneSamplerBindingsRejectDuplicateSlots)
{
    CreateTestShaderProgram(device, "duplicate_sampler_slots");
    RHISampler* sampler = device->CreateSampler({});
    RenderGraph graph("duplicate_sampler_slots");
    graph.Begin();

    RDGComputePassDesc pass;
    pass.SetShaderProgramName("duplicate_sampler_slots");
    pass.BindSampler("uSamplerHeap", sampler, 3);
    pass.BindSampler("uSamplerHeap", sampler, 3);
    graph.AddComputePass(std::move(pass));
    EXPECT_FALSE(graph.End());
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eBinding);
}

TEST_F(RenderCoreTest, UnlimitedPointAndWideSpotShadowsUseFiniteSceneDepth)
{
    sg::Scene source;
    source.GetAABB() = sg::AABB(Vec3(-1), Vec3(1));
    SceneData data{};
    data.pScene = &source;
    RenderScene         scene(device, data);
    SceneShadowRenderer shadows(device);
    sceneInputs.uniforms.lightInfo.x = 1.0f;

    for (float type : {1.0f, 2.0f})
    {
        sceneInputs.uniforms.lights[0] = {Vec4(0, 0, 2, 0), Vec4(0, 0, -1, type), Vec4(1), Vec4(1, 0, 1, 0)};
        ASSERT_TRUE(shadows.Prepare(scene, true));
        RDGComputePassDesc lighting;
        shadows.BindLightingInputs(lighting);
        SceneShadowUniformData uniforms;
        ASSERT_EQ(lighting.valueByteStorage.size(), sizeof(uniforms));
        std::memcpy(&uniforms, lighting.valueByteStorage.data(), sizeof(uniforms));
        EXPECT_GT(uniforms.lights[0].z, 0.0f);
        for (uint32_t column = 0; column < 4; ++column)
        {
            for (uint32_t row = 0; row < 4; ++row)
            {
                EXPECT_TRUE(std::isfinite(uniforms.viewProjection[0][column][row]));
            }
        }
    }

    shadows.Destroy();
}
