TEST_F(RenderCoreTest, VoxelRevisionPublishesOnlyAfterSuccessfulHandoff)
{
    TestVoxelVolumes volumes(device, DataFormat::eR8G8B8A8UNORM);
    volumes.Init();
    RenderGraph graph("voxel_revision_publication");
    ASSERT_TRUE(graph.Begin());
    EXPECT_TRUE(volumes.BeginVolumeUpdate(graph));
    EXPECT_EQ(volumes.GetGeometryRevision(), 0u);
    EXPECT_EQ(volumes.GetRecordedGeometryRevision(), 1u);
    ASSERT_TRUE(graph.End());
    volumes.OnRenderGraphExecuted(false);
    EXPECT_EQ(volumes.GetGeometryRevision(), 0u);
    EXPECT_EQ(volumes.GetRecordedGeometryRevision(), 0u);

    ASSERT_TRUE(graph.Begin());
    EXPECT_TRUE(volumes.BeginVolumeUpdate(graph));
    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    volumes.OnRenderGraphExecuted(true);
    EXPECT_EQ(volumes.GetGeometryRevision(), 1u);
    volumes.OnRenderGraphExecuted(true);
    EXPECT_EQ(volumes.GetGeometryRevision(), 1u);
    volumes.Destroy();
}

TEST_F(RenderCoreTest, VoxelOutputsAllocateLazilyAndKeepTheirConfiguredClass)
{
    TestVoxelVolumes volumes(device, DataFormat::eR8G8B8A8UNORM, false, 64);
    EXPECT_FALSE(volumes.IsReady());
    EXPECT_EQ(volumes.GetVoxelTextures().pOwner, nullptr);
    EXPECT_FALSE(volumes.ConfigureClass(0));
    ASSERT_TRUE(volumes.ConfigureClass(GI_DYNAMIC));
    ASSERT_TRUE(volumes.EnsureReady());
    RHITexture* owner          = volumes.GetVoxelTextures().pOwner;
    const uint32_t allocations = rhi->textureCreations;
    EXPECT_TRUE(volumes.EnsureReady());
    EXPECT_EQ(rhi->textureCreations, allocations);
    EXPECT_EQ(volumes.GetVoxelTextures().pOwner, owner);
    EXPECT_FALSE(volumes.ConfigureClass(GI_STATIC));
    EXPECT_EQ(volumes.GetClassMask(), GI_DYNAMIC);
    ASSERT_TRUE(volumes.EnableCompaction());
    RHIBuffer* list = volumes.GetOccupiedList();
    EXPECT_TRUE(volumes.EnableCompaction());
    EXPECT_EQ(volumes.GetOccupiedList(), list);
    EXPECT_EQ(list->GetRequiredSize(), 64u * 64 * 64 * sizeof(uint32_t));
    EXPECT_EQ(volumes.GetGridToList()->GetRequiredSize(), list->GetRequiredSize());
    EXPECT_EQ(volumes.GetOccupiedCount()->GetRequiredSize(), sizeof(uint32_t));
    volumes.Destroy();
}

TEST_F(RenderCoreTest, EmptyVoxelInputClearsAllSurfaceOutputsWithoutSceneBindings)
{
    sg::Scene source;
    source.GetAABB() = sg::AABB(Vec3(-1.0f), Vec3(1.0f));
    SceneData data{};
    data.pScene = &source;
    RenderScene scene(device, data);
    EXPECT_EQ(scene.GetVoxelTriangleBuffer(), nullptr);
    EXPECT_EQ(scene.GetVoxelTriangleCount(), 0u);
    TestVoxelVolumes volumes(device, DataFormat::eR8G8B8A8UNORM, true);
    volumes.Init();
    volumes.SetRenderScene(&scene);
    RenderGraph* graph = device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph->Begin());
    EXPECT_TRUE(volumes.BeginVolumeUpdate(*graph));
    volumes.ResolveVolume();
    ASSERT_TRUE(graph->End());
    ASSERT_TRUE(device->ExecuteRenderGraph(*graph)) << graph->GetResult().message;
    volumes.OnRenderGraphExecuted(true);
    EXPECT_EQ(volumes.GetGeometryRevision(), 1u);
    ASSERT_EQ(rhi->graphics.textureClears.size(), 3u);
    const VoxelTextures& textures = volumes.GetVoxelTextures();
    EXPECT_EQ(rhi->graphics.textureClears[0].texture, textures.pAlbedo);
    EXPECT_EQ(rhi->graphics.textureClears[1].texture, textures.pNormal);
    EXPECT_EQ(rhi->graphics.textureClears[2].texture, textures.pEmissive);
    volumes.Destroy();
}

TEST_F(RenderCoreTest, AveragedReflectanceClearsAndRetriesWithoutPublishingFailedGeneration)
{
    sg::Scene source;
    source.GetAABB() = sg::AABB(Vec3(-1), Vec3(1));
    SceneData data{};
    data.pScene = &source;
    RenderScene scene(device, data);
    TestVoxelVolumes volumes(device, DataFormat::eR8G8B8A8UNORM, true, 64);
    volumes.Init();
    volumes.EnableAveragedReflectanceForTest();
    volumes.SetRenderScene(&scene);
    RenderGraph& graph = *device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph.Begin());
    ASSERT_TRUE(volumes.BeginVolumeUpdate(graph));
    ASSERT_TRUE(volumes.UsesAveragedReflectance());
    RHIBuffer* sums    = volumes.GetReflectanceSums();
    RHITexture* output = volumes.GetVoxelTextures().pReflectance;
    ASSERT_NE(sums, nullptr);
    ASSERT_NE(output, nullptr);
    volumes.ResolveVolume();
    ASSERT_TRUE(graph.End());
    volumes.OnRenderGraphExecuted(false);
    EXPECT_EQ(volumes.GetGeometryRevision(), 0u);
    ASSERT_TRUE(graph.Begin());
    ASSERT_TRUE(volumes.BeginVolumeUpdate(graph));
    volumes.ResolveVolume();
    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
    volumes.OnRenderGraphExecuted(true);
    EXPECT_EQ(volumes.GetGeometryRevision(), 1u);
    EXPECT_EQ(volumes.GetReflectanceSums(), sums);
    EXPECT_EQ(volumes.GetVoxelTextures().pReflectance, output);
    ASSERT_EQ(rhi->graphics.textureClears.size(), 4u);
    EXPECT_EQ(rhi->graphics.textureClears.back().texture, output);
    volumes.Destroy();
}

TEST_F(RenderCoreTest, VoxelGIFailedAllocationsPreserveVisualizationAndAllowRetry)
{
    for (uint32_t failure = 1; failure <= 4; ++failure)
    {
        SCOPED_TRACE(failure);
        TestVoxelVolumes volumes(device, DataFormat::eR8G8B8A8UNORM);
        volumes.Init();
        RHITexture* albedo = volumes.GetVoxelTextures().pAlbedo;
        VoxelGIRenderer gi(device, &volumes);
        const size_t firstAllocation = rhi->createdTextureIds.size();
        rhi->failTextureCreationAt   = rhi->textureCreations + failure;
        EXPECT_FALSE(gi.Init());
        EXPECT_FALSE(gi.IsInitialized());
        EXPECT_FALSE(volumes.ProducesRadianceInputs());
        EXPECT_TRUE(volumes.IsReady());
        EXPECT_EQ(volumes.GetVoxelTextures().pAlbedo, albedo);
        device->CollectCompletedResources();
        for (size_t i = firstAllocation; i < rhi->createdTextureIds.size(); ++i)
        {
            EXPECT_TRUE(destroyed.contains(rhi->createdTextureIds[i]));
        }
        rhi->failTextureCreationAt = 0;
        ASSERT_TRUE(gi.Init());
        EXPECT_TRUE(volumes.ProducesRadianceInputs());
        gi.Destroy();
        volumes.Destroy();
        device->CollectCompletedResources();
    }
}

TEST_F(RenderCoreTest, VoxelGIFailedMipViewsCanRetryWithoutDuplicatingAlbedoViews)
{
    for (uint32_t failure = 1; failure <= 8; ++failure)
    {
        SCOPED_TRACE(failure);
        TestVoxelVolumes volumes(device, DataFormat::eR8G8B8A8UNORM);
        volumes.Init();
        VoxelGIRenderer gi(device, &volumes);
        rhi->failTextureViewCreationAt = rhi->textureViewCreations + failure;
        EXPECT_FALSE(gi.Init());
        EXPECT_FALSE(gi.IsInitialized());
        EXPECT_FALSE(volumes.ProducesRadianceInputs());
        EXPECT_TRUE(volumes.IsReady());
        const uint32_t beforeRetry     = rhi->textureViewCreations;
        rhi->failTextureViewCreationAt = 0;
        ASSERT_TRUE(gi.Init());
        const uint32_t retainedAlbedoViews = failure > 4 ? failure - 5 : 0;
        EXPECT_EQ(rhi->textureViewCreations - beforeRetry, 8 - retainedAlbedoViews);
        const uint32_t initializedViews = rhi->textureViewCreations;
        EXPECT_TRUE(gi.Init());
        EXPECT_EQ(rhi->textureViewCreations, initializedViews);
        gi.Destroy();
        volumes.Destroy();
        device->CollectCompletedResources();
    }
}

static uint32_t CountGIPasses(const RDGMetricsSnapshot& snapshot, std::string_view prefix)
{
    uint32_t count = 0;
    for (const RDGNodeMetrics& node : snapshot.nodes)
    {
        if (std::string_view(node.name.CStr()).starts_with(prefix))
        {
            ++count;
        }
    }
    return count;
}

TEST_F(RenderCoreTest, VoxelGISettingsInvalidateOnlyDependentPasses)
{
    for (const std::array<const char*, 2>& shader :
         {std::array<const char*, 2>{"VoxelFilterAlbedoSP", "VoxelGI/filter_albedo.comp.spv"},
          {"VoxelFilterRadianceSP", "VoxelGI/filter_radiance.comp.spv"},
          {"VoxelSkyIrradianceSP", "VoxelGI/sky_irradiance.comp.spv"},
          {"VoxelInjectRadianceSP", "VoxelGI/inject_radiance.comp.spv"}})
    {
        RHIShaderCreateInfo info;
        info.stageFlags.SetFlag(RHIShaderStageFlagBits::eCompute);
        info.spirvFileName[ToUnderlying(RHIShaderStage::eCompute)] = shader[1];
        reflectedShaderInfos[shader[0]]                            = info;
        CreateTestShaderProgram(device, shader[0]);
    }
    CreateTestShaderProgram(device, "intent");
    rc::TextureFormat environmentFormat;
    environmentFormat.dimension   = TextureDimension::eCube;
    environmentFormat.arrayLayers = 6;
    environmentFormat.width = environmentFormat.height = 8;
    environmentFormat.format                           = DataFormat::eR16G16B16A16SFloat;
    sceneInputs.environment.pSkybox =
        device->CreateTextureSampled(environmentFormat, {.copyUsage = true}, "sky");
    sceneInputs.environment.pPrefilteredSampler = device->CreateSampler({});
    sg::Scene source;
    source.GetAABB() = sg::AABB(Vec3(-1.0f), Vec3(1.0f));
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
    ASSERT_TRUE(gi.SetSettings(settings));
    RDGMetrics& metrics = device->GetRDGMetrics();
    metrics.SetSink({});
    RenderGraph* graph = device->GetCurrentFrameRDG();

    float environmentIntensity = 1.0f;
    float environmentRotation  = 0.0f;
    bool environmentEnabled    = true;
    bool skyboxVisible         = true;
    for (uint32_t frame = 0; frame < 24; ++frame)
    {
        SCOPED_TRACE(frame);
        switch (frame)
        {
            case 2: settings.indirectIntensity = 2.0f; break;
            case 3: settings.coneAngleDegrees = 45.0f; break;
            case 4: settings.stepScale = 0.5f; break;
            case 5: settings.maxDistanceGridLengths = 1.0f; break;
            case 6: settings.maxSteps = 64; break;
            case 7: settings.shadows = false; break;
            case 8: settings.coneCount = 4; break;
            case 9: settings.normalBiasVoxels = 2.0f; break;
            case 10:
            {
                VoxelGISettings invalid = settings;
                invalid.coneCount       = 5;
                EXPECT_FALSE(gi.SetSettings(invalid));
                break;
            }
            case 11: EXPECT_NE(scene.GetLights().Add({}), 0u); break;
            case 12: volumes.RequestVoxelization(); break;
            case 14: skyboxVisible = false; break;
            case 15: skyboxVisible = true; break;
            case 16: environmentIntensity = 0.25f; break;
            case 17: environmentRotation = 90.0f; break;
            case 18: environmentEnabled = false; break;
            case 19: skyboxVisible = false; break;
            case 20:
            {
                const uint64_t revision = scene.GetEnvironmentRevision();
                const Vec4 previous     = sceneInputs.uniforms.environment;
                EXPECT_FALSE(scene.SetEnvironmentLighting(-1, 0, true, true));
                EXPECT_FALSE(scene.SetEnvironmentLighting(1, std::numeric_limits<float>::infinity(),
                                                          true, true));
                EXPECT_FALSE(scene.SetEnvironmentLighting(std::numeric_limits<float>::quiet_NaN(),
                                                          0, true, true));
                EXPECT_EQ(scene.GetEnvironmentRevision(), revision);
                const SceneUniformData* uniforms =
                    reinterpret_cast<const SceneUniformData*>(scene.GetSceneUniformData());
                EXPECT_EQ(uniforms->environment, previous);
                break;
            }
            case 21: environmentEnabled = true; break;
            default: break;
        }
        EXPECT_TRUE(gi.SetSettings(settings)); // Also covers repeated unchanged settings.
        const uint64_t environmentRevision = scene.GetEnvironmentRevision();
        EXPECT_TRUE(scene.SetEnvironmentLighting(environmentIntensity, environmentRotation,
                                                 environmentEnabled, skyboxVisible));
        const bool environmentChanged = frame == 16 || frame == 17 || frame == 18 || frame == 21;
        EXPECT_EQ(scene.GetEnvironmentRevision(),
                  environmentRevision + (environmentChanged ? 1 : 0));
        const SceneUniformData* uniforms =
            reinterpret_cast<const SceneUniformData*>(scene.GetSceneUniformData());
        EXPECT_EQ(uniforms->environment,
                  Vec4(environmentIntensity, glm::radians(environmentRotation),
                       environmentEnabled ? 1.0f : 0.0f, skyboxVisible ? 1.0f : 0.0f));
        ASSERT_TRUE(graph->Begin());
        const bool geometryChanged = volumes.BeginVolumeUpdate(*graph);
        if (geometryChanged)
        {
            const VoxelTextures& textures = volumes.GetVoxelTextures();
            for (RHITexture* texture : {textures.pAlbedo, textures.pNormal, textures.pEmissive})
            {
                graph->AddTransferPass("InitializeVoxelAttributes").ClearTexture(texture, Color(0));
            }
        }
        if (frame == 0)
        {
            graph->AddTransferPass("InitializeSky")
                .ClearTexture(sceneInputs.environment.pSkybox, Color(0));
        }
        gi.BuildRenderGraph();
        graph->AddComputePass(IntentPass("keepalive"));
        metrics.RequestCapture();
        ASSERT_TRUE(graph->End());
        ASSERT_TRUE(device->ExecuteRenderGraph(*graph)) << graph->GetResult().message;
        gi.OnRenderGraphExecuted(true);
        volumes.OnRenderGraphExecuted(true);
        const bool skyChanged = geometryChanged || environmentChanged || frame == 8 || frame == 9;
        const bool radianceChanged         = skyChanged || frame == 7 || frame == 11;
        const RDGMetricsSnapshot& snapshot = metrics.GetLastSnapshot();
        EXPECT_EQ(CountGIPasses(snapshot, "VoxelOpacityMip"), geometryChanged ? 3u : 0u);
        EXPECT_EQ(CountGIPasses(snapshot, "VoxelSkyIrradiance"), skyChanged ? 1u : 0u);
        EXPECT_EQ(CountGIPasses(snapshot, "VoxelInjectRadiance"), radianceChanged ? 1u : 0u);
        EXPECT_EQ(CountGIPasses(snapshot, "VoxelRadianceMip"), radianceChanged ? 3u : 0u);
    }
    gi.Destroy();
    volumes.Destroy();
    device->DestroyTexture(sceneInputs.environment.pSkybox);
}
