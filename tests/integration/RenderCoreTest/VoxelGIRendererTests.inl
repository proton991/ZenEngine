TEST_F(RenderCoreTest, VoxelDrawRecordsIncludeBlendAndTransmissionWithoutChangingReceiverCoverage)
{
    sg::Scene                           source;
    HeapVector<UniquePtr<sg::Material>> materials;
    HeapVector<UniquePtr<sg::SubMesh>>  primitives;
    sg::Mesh                            mesh("occluders");
    for (uint32_t i = 0; i < 4; ++i)
    {
        UniquePtr<sg::Material> material        = MakeUnique<sg::Material>("surface");
        material->index                         = i;
        material->data.surfaceProperties.y      = i == 0 ? static_cast<float>(sg::AlphaMode::Blend)
                                                : i == 2 ? static_cast<float>(sg::AlphaMode::Mask)
                                                         : static_cast<float>(sg::AlphaMode::Opaque);
        material->data.sheenColorTransmission.w = i == 1 ? 1.0f : 0.0f;
        UniquePtr<sg::SubMesh> primitive        = MakeUnique<sg::SubMesh>("triangle", i * 3, 3, 3);
        primitive->SetMaterial(i, material.Get());
        mesh.AddSubMesh(primitive.Get());
        primitives.push_back(std::move(primitive));
        materials.push_back(std::move(material));
    }
    sg::Node node(0, "occluders");
    node.AddComponent(&mesh);
    source.AddRenderableNode(&node);
    SceneData data{};
    data.pScene = &source;
    RenderScene                     scene(device, data);
    const HeapVector<SceneMeshDraw> occluders = SnapshotSceneDraws(scene, GI_ALL, true, false);
    ASSERT_EQ(occluders.size(), 4u);
    for (uint32_t i = 0; i < occluders.size(); ++i)
    {
        EXPECT_EQ(occluders[i].firstTriangle, i);
        EXPECT_EQ(occluders[i].materialIndex, i);
    }
    const HeapVector<SceneMeshDraw> receivers = SnapshotSceneDraws(scene);
    ASSERT_EQ(receivers.size(), 2u);
    EXPECT_EQ(receivers[0].firstTriangle, 2u);
    EXPECT_EQ(receivers[1].firstTriangle, 3u);
}

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
    volumes.OnRenderGraphExecuted(RenderFrameResult::eFailed);
    EXPECT_EQ(volumes.GetGeometryRevision(), 0u);
    EXPECT_EQ(volumes.GetRecordedGeometryRevision(), 0u);

    ASSERT_TRUE(graph.Begin());
    EXPECT_TRUE(volumes.BeginVolumeUpdate(graph));
    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    volumes.OnRenderGraphExecuted(RenderFrameResult::eSubmitted);
    EXPECT_EQ(volumes.GetGeometryRevision(), 1u);
    volumes.OnRenderGraphExecuted(RenderFrameResult::eSubmitted);
    EXPECT_EQ(volumes.GetGeometryRevision(), 1u);
    volumes.Destroy();
}

TEST_F(RenderCoreTest, VoxelOutputsAllocateLazily)
{
    TestVoxelVolumes volumes(device, DataFormat::eR8G8B8A8UNORM, false, 64);
    EXPECT_FALSE(volumes.IsReady());
    EXPECT_EQ(volumes.GetVoxelTextures().pOwner, nullptr);
    ASSERT_TRUE(volumes.EnsureReady());
    RHITexture*    owner       = volumes.GetVoxelTextures().pOwner;
    const uint32_t allocations = rhi->textureCreations;
    EXPECT_TRUE(volumes.EnsureReady());
    EXPECT_EQ(rhi->textureCreations, allocations);
    EXPECT_EQ(volumes.GetVoxelTextures().pOwner, owner);
    volumes.Destroy();
}

TEST_F(RenderCoreTest, VoxelClassOwnersStackSurfaceClassesUpToTheLightCacheResolution)
{
    for (uint32_t dimension : {8u, 256u})
    {
        SCOPED_TRACE(dimension);
        TestVoxelVolumes volumes(device, DataFormat::eR8G8B8A8UNORM, true, dimension);
        volumes.Init();
        RHITexture* classOwner = volumes.GetVoxelTextures().pClassOwner;
        ASSERT_NE(classOwner, nullptr);
        const uint32_t resolution = std::min(dimension, ZEN_LIGHT_CACHE_MAX_RESOLUTION);
        EXPECT_EQ(classOwner->GetFormat(), DataFormat::eR32UInt);
        EXPECT_EQ(classOwner->GetWidth(), resolution);
        EXPECT_EQ(classOwner->GetHeight(), resolution);
        EXPECT_EQ(classOwner->GetDepth(), resolution * ZEN_VOXEL_SURFACE_CLASSES);
        volumes.Destroy();
    }
    // Albedo-only producers bind a placeholder and elect no class owners.
    TestVoxelVolumes visualization(device, DataFormat::eR8G8B8A8UNORM, false, 64);
    ASSERT_TRUE(visualization.EnsureReady());
    EXPECT_EQ(visualization.GetVoxelTextures().pClassOwner, nullptr);
    visualization.Destroy();
}

TEST_F(RenderCoreTest, VoxelGIFailedSurfaceAllocationStaysLatchedUntilReplacement)
{
    for (uint32_t failure = 1; failure <= 2; ++failure)
    {
        SCOPED_TRACE(failure);

        TestVoxelVolumes volumes(device, DataFormat::eR8G8B8A8UNORM, false, 64);

        EXPECT_FALSE(volumes.HasFailedInitialization());

        const size_t firstAllocation = rhi->createdTextureIds.size();

        rhi->failTextureCreationAt   = rhi->textureCreations + failure;

        EXPECT_FALSE(volumes.EnsureReady());
        EXPECT_TRUE(volumes.HasFailedInitialization());

        rhi->failTextureCreationAt = 0;

        const uint32_t allocations = rhi->textureCreations;

        EXPECT_FALSE(volumes.EnsureReady());
        EXPECT_EQ(rhi->textureCreations, allocations);

        volumes.Destroy();
        device->CollectCompletedResources();

        for (size_t i = firstAllocation; i < rhi->createdTextureIds.size(); ++i)
        {
            EXPECT_TRUE(destroyed.contains(rhi->createdTextureIds[i]));
        }

        TestVoxelVolumes replacement(device, DataFormat::eR8G8B8A8UNORM, false, 64);

        ASSERT_TRUE(replacement.EnsureReady());
        EXPECT_FALSE(replacement.HasFailedInitialization());

        replacement.Destroy();
    }
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
    volumes.OnRenderGraphExecuted(RenderFrameResult::eSubmitted);
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
    RenderScene      scene(device, data);
    TestVoxelVolumes volumes(device, DataFormat::eR8G8B8A8UNORM, true, 64);
    volumes.Init();
    volumes.EnableAveragedReflectanceForTest();
    volumes.SetRenderScene(&scene);
    RenderGraph& graph = *device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph.Begin());
    ASSERT_TRUE(volumes.BeginVolumeUpdate(graph));
    ASSERT_TRUE(volumes.UsesAveragedReflectance());
    RHIBuffer*  sums   = volumes.GetReflectanceSums();
    RHITexture* output = volumes.GetVoxelTextures().pReflectance;
    ASSERT_NE(sums, nullptr);
    ASSERT_NE(output, nullptr);
    volumes.ResolveVolume();
    ASSERT_TRUE(graph.End());
    volumes.OnRenderGraphExecuted(RenderFrameResult::eFailed);
    EXPECT_EQ(volumes.GetGeometryRevision(), 0u);
    ASSERT_TRUE(graph.Begin());
    ASSERT_TRUE(volumes.BeginVolumeUpdate(graph));
    volumes.ResolveVolume();
    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
    volumes.OnRenderGraphExecuted(RenderFrameResult::eSubmitted);
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
        RHITexture*     albedo = volumes.GetVoxelTextures().pAlbedo;
        VoxelGIRenderer gi(device, &volumes);
        const size_t    firstAllocation = rhi->createdTextureIds.size();
        rhi->failTextureCreationAt      = rhi->textureCreations + failure;
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
    // The fallback array view precedes four radiance and four albedo mip views.
    for (uint32_t failure = 1; failure <= 9; ++failure)
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
        const uint32_t retainedAlbedoViews = failure > 5 ? failure - 6 : 0;
        EXPECT_EQ(rhi->textureViewCreations - beforeRetry, 9 - retainedAlbedoViews);
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
          {"EnvironmentColumnsSP", "VoxelGI/environment_columns.comp.spv"},
          {"EnvironmentRowsSP", "VoxelGI/environment_rows.comp.spv"},
          {"EnvironmentTilesSP", "VoxelGI/environment_tiles.comp.spv"},
          {"VoxelFilterRadianceSP", "VoxelGI/filter_radiance.comp.spv"},
          {"VoxelInjectRadianceSP", "VoxelGI/inject_radiance.comp.spv"}})
    {
        RHIShaderCreateInfo info;
        info.stageFlags.SetFlag(RHIShaderStageFlagBits::eCompute);
        info.spirvFileName[ToUnderlying(RHIShaderStage::eCompute)] = shader[1];
        RegisterReflectedShader(shader[0], info);
        CreateTestShaderProgram(device, shader[0]);
    }
    CreateTestShaderProgram(device, "intent");
    rc::TextureFormat environmentFormat;
    environmentFormat.dimension   = TextureDimension::eCube;
    environmentFormat.arrayLayers = 6;
    environmentFormat.width = environmentFormat.height = 8;
    environmentFormat.format                           = DataFormat::eR16G16B16A16SFloat;
    sceneInputs.environment.pPrefiltered        = device->CreateTextureSampled(environmentFormat, {.copyUsage = true}, "sky");
    sceneInputs.environment.pPrefilteredSampler = device->CreateSampler({});
    sg::Scene source;
    source.GetAABB() = sg::AABB(Vec3(-1.0f), Vec3(1.0f));
    SceneData data{};
    data.pScene = &source;
    RenderScene      scene(device, data);
    TestVoxelVolumes volumes(device, DataFormat::eR8G8B8A8UNORM);
    volumes.Init();
    volumes.SetRenderScene(&scene);
    VoxelGIRenderer gi(device, &volumes);
    gi.SetRenderScene(&scene);
    ASSERT_TRUE(gi.Init());
    VoxelGISettings settings;
    ASSERT_TRUE(gi.SetSettings(settings));
    RDGMetrics&       metrics          = device->GetRDGMetrics();
    RDGMetricsOptions metricOptions    = metrics.GetOptions();
    metricOptions.includeTransferNodes = true;
    metricOptions.maxNodeDetails       = 512;
    metrics.Configure(metricOptions);
    metrics.SetSink({});
    RenderGraph* graph         = device->GetCurrentFrameRDG();

    float environmentIntensity = 1.0f;
    float environmentRotation  = 0.0f;
    bool  environmentEnabled   = true;
    bool  skyboxVisible        = true;
    for (uint32_t frame = 0; frame < 36; ++frame)
    {
        SCOPED_TRACE(frame);
        switch (frame)
        {
            case 2: settings.indirectIntensity = 2.0f; break;
            case 7: settings.shadows = false; break;
            case 9: settings.normalBiasVoxels = 2.0f; break;
            case 10:
            {
                VoxelGISettings invalid = settings;
                invalid.historyFrames   = 0;
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
                const Vec4     previous = sceneInputs.uniforms.environment;
                EXPECT_FALSE(scene.SetEnvironmentLighting(-1, 0, true, true));
                EXPECT_FALSE(scene.SetEnvironmentLighting(1, std::numeric_limits<float>::infinity(), true, true));
                EXPECT_FALSE(scene.SetEnvironmentLighting(std::numeric_limits<float>::quiet_NaN(), 0, true, true));
                EXPECT_EQ(scene.GetEnvironmentRevision(), revision);
                const SceneUniformData* uniforms = reinterpret_cast<const SceneUniformData*>(scene.GetSceneUniformData());
                EXPECT_EQ(uniforms->environment, previous);
                break;
            }
            case 21: environmentEnabled = true; break;
            case 24: settings.analyticLighting = false; break;
            case 25: settings.environmentLighting = false; break;
            case 26: settings.emissiveLighting = false; break;
            case 28: settings.analyticLighting = true; break;
            case 29: settings.environmentLighting = true; break;
            case 30: settings.emissiveLighting = true; break;
            case 32: settings.rtShadows = false; break;
            case 33: settings.rtShadows = true; break;
            case 34: settings.rtShadows = false; break;
            default: break;
        }
        EXPECT_TRUE(gi.SetSettings(settings)); // Also covers repeated unchanged settings.
        const uint64_t environmentRevision = scene.GetEnvironmentRevision();
        EXPECT_TRUE(scene.SetEnvironmentLighting(environmentIntensity, environmentRotation, environmentEnabled, skyboxVisible));
        const bool environmentChanged = frame == 16 || frame == 17 || frame == 18 || frame == 21;
        EXPECT_EQ(scene.GetEnvironmentRevision(), environmentRevision + (environmentChanged ? 1 : 0));
        const SceneUniformData* uniforms = reinterpret_cast<const SceneUniformData*>(scene.GetSceneUniformData());
        EXPECT_EQ(uniforms->environment, Vec4(environmentIntensity, glm::radians(environmentRotation),
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
            graph->AddTransferPass("InitializeSky").ClearTexture(sceneInputs.environment.pPrefiltered, Color(0));
        }
        gi.BuildRayQueryGraph();
        EXPECT_FALSE(gi.UsesHardwareShadows()); // This device has no queries: an explicit request must still fall back.
        // A successful query-only frame (for example shadow-map preparation falling
        // back to PBR) must not publish an invalidated but unrecorded radiance cache.
        if (frame != 34)
        {
            gi.BuildRenderGraph();
        }
        graph->AddComputePass(IntentPass("keepalive"));
        metrics.RequestCapture();
        ASSERT_TRUE(graph->End());
        ASSERT_TRUE(device->ExecuteRenderGraph(*graph)) << graph->GetResult().message;
        gi.OnRenderGraphExecuted(RenderFrameResult::eSubmitted);
        volumes.OnRenderGraphExecuted(RenderFrameResult::eSubmitted);
        const bool skyChanged      = geometryChanged || environmentChanged || frame == 9 || frame == 25 || frame == 29;
        const bool radianceChanged = skyChanged || frame == 7 || frame == 11 || frame == 24 || frame == 26 || frame == 28
                                  || frame == 30 || (frame >= 32 && frame != 34);
        const RDGMetricsSnapshot& snapshot = metrics.GetLastSnapshot();
        EXPECT_EQ(CountGIPasses(snapshot, "VoxelOpacityMip"), geometryChanged ? 3u : 0u);
        EXPECT_EQ(CountGIPasses(snapshot, "VoxelSkyIrradiance"), skyChanged ? 1u : 0u);
        EXPECT_EQ(CountGIPasses(snapshot, "VoxelInjectRadiance"), radianceChanged ? 1u : 0u);
        EXPECT_EQ(CountGIPasses(snapshot, "VoxelRadianceMip"), radianceChanged ? 3u : 0u);
    }
    gi.Destroy();
    volumes.Destroy();
    device->DestroyTexture(sceneInputs.environment.pPrefiltered);
}

// Compute GI waits for the distance field, built on the GPU in bounded per-frame work
// (VoxelGIRenderer::IsPreparingField). Tests that trace on a prepared frame poll until it is ready.
bool BuildPreparedRayQueryGraph(VoxelGIRenderer& gi)
{
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    bool                                        ready    = gi.BuildRayQueryGraph();
    while (!ready && gi.IsPreparingField() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ready = gi.BuildRayQueryGraph();
    }
    return ready;
}

// Field construction programs on this fake device, which reports desktop compute limits. The scene
// needs whole vertex records and node, material and UV buffers to snapshot. Scenes here have
// no triangles, so their field publishes once its clearing submission retires.
void EnableDistanceFieldTestDevice(RenderDevice* device)
{
    const char* const programs[][2] = {{"DistanceFieldSnapshotSP", "distance_field_snapshot"},
                                       {"DistanceFieldSnapshotVerticesSP", "distance_field_snapshot_vertices"},
                                       {"DistanceFieldClearSP", "distance_field_clear"},
                                       {"DistanceFieldPrepareSP", "distance_field_prepare"},
                                       {"DistanceFieldAdvanceSP", "distance_field_advance"},
                                       {"DistanceFieldBatchClearSP", "distance_field_batch_clear"},
                                       {"DistanceFieldJobsSP", "distance_field_jobs"},
                                       {"DistanceFieldCoverageSP", "distance_field_coverage"},
                                       {"DistanceFieldCountSP", "distance_field_count"},
                                       {"DistanceFieldScanSP", "distance_field_scan"},
                                       {"DistanceFieldScanSumsSP", "distance_field_scan_sums"},
                                       {"DistanceFieldAllocateSP", "distance_field_allocate"},
                                       {"DistanceFieldCompactSP", "distance_field_compact"},
                                       {"DistanceFieldEmitSP", "distance_field_emit"},
                                       {"DistanceFieldHierarchySP", "distance_field_hierarchy"}};
    for (const char* const(&program)[2] : programs)
    {
        if (ShaderProgramManager::GetInstance().RequestShaderProgram(program[0]) == nullptr)
        {
            RHIShaderCreateInfo shader;
            shader.stageFlags.SetFlag(RHIShaderStageFlagBits::eCompute);
            shader.spirvFileName[ToUnderlying(RHIShaderStage::eCompute)] = std::string("VoxelGI/") + program[1] + ".comp.spv";
            RegisterReflectedShader(program[0], shader);
            CreateTestShaderProgram(device, program[0]);
        }
    }
    if (ShaderProgramManager::GetInstance().RequestShaderProgram("intent") == nullptr)
    {
        CreateTestShaderProgram(device, "intent");
    }
}

TEST_F(RenderCoreTest, DistanceFieldDerivedCapacitiesFitNativeAllocationBudget)
{
    sceneInputs.vertices  = Buffer(sizeof(asset::Vertex));
    sceneInputs.indices   = Buffer();
    sceneInputs.nodes     = Buffer();
    sceneInputs.materials = Buffer();
    sceneInputs.uv        = Buffer();
    // The generation's separately allocated range table must be charged alongside shared scratch.
    // Its 4,352-byte native allocation exceeds any incidental slack between capacity steps.
    for (uint32_t instance = 0; instance < 257; ++instance)
    {
        sceneInputs.shadowCasters.push_back({sg::AABB(Vec3(-1), Vec3(1)), 1});
        sceneInputs.triangleRanges.push_back({instance * 100, 100, GI_STATIC, Vec4(0)});
    }
    EnableDistanceFieldTestDevice(device);
    sg::Scene source;
    source.GetAABB() = sg::AABB(Vec3(-1), Vec3(1));
    SceneData data{};
    data.pScene = &source;
    RenderScene scene(device, data);
    // This fixture injects GPU records without scene nodes; retain its authored synthetic bounds.
    source.GetAABB() = sg::AABB(Vec3(-1), Vec3(1));
    ASSERT_EQ(scene.GetVoxelTriangleCount(), 25700u);
    DistanceFieldSceneSettings settings;
    settings.cellsPerAxis       = 4;
    settings.atlasTexelsPerAxis = 8;
    settings.jobCapacity        = 1;
    settings.referenceCapacity  = 1;
    settings.deriveCapacities   = true;
    // Device headroom must not replace the explicit scratch limit.
    rhi->info.deviceLocalMemoryBytes = 8ull * 1024 * 1024 * 1024;
    rhi->bufferAllocationAlignment   = 256;
    rhi->bufferAllocationPadding     = 0;
    for (uint32_t scenario = 0; scenario < 3; ++scenario)
    {
        SCOPED_TRACE(scenario);
        // The proportional payload estimate fills this budget, but every native buffer rounds up.
        // Also verify that an unaffordable floor and a failed requirements query allocate nothing.
        settings.scratchBudgetBytes    = scenario == 1 ? 1 : 65539;
        rhi->failBufferAllocationQuery = scenario == 2;
        DistanceFieldScene field(device, settings);
        const uint32_t     creations = rhi->bufferCreations;
        RenderGraph&       graph     = *device->GetCurrentFrameRDG();
        ASSERT_TRUE(graph.Begin());
        const DistanceFieldScene::State state = field.BuildRenderGraph(scene);
        if (scenario == 0)
        {
            ASSERT_NE(state, DistanceFieldScene::State::Failed) << field.GetFailureReason();
            EXPECT_LE(field.GetScratchBytes(), settings.scratchBudgetBytes);
            EXPECT_GT(field.GetScratchBytes(), settings.scratchBudgetBytes - 4096);
            uint32_t rangeBuffers = 0;
            for (const RHIBufferCreateInfo& info : rhi->createdBufferInfos)
            {
                if (info.tag == "field_instance_ranges")
                {
                    EXPECT_EQ(info.size, 257 * sizeof(glm::uvec4));
                    ++rangeBuffers;
                }
            }
            EXPECT_EQ(rangeBuffers, 1u);
        }
        else
        {
            EXPECT_EQ(state, DistanceFieldScene::State::Failed);
            EXPECT_EQ(rhi->bufferCreations, creations);
        }
        graph.AddComputePass(IntentPass("keepalive"));
        ASSERT_TRUE(graph.End()) << graph.GetResult().message;
        ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
        field.OnRenderGraphExecuted(RenderFrameResult::eSubmitted);
        field.Destroy();
        device->NextFrame();
    }
    rhi->failBufferAllocationQuery = false;
    for (RHIBuffer* buffer :
         {sceneInputs.vertices, sceneInputs.indices, sceneInputs.nodes, sceneInputs.materials, sceneInputs.uv})
    {
        device->DestroyBuffer(buffer);
    }
    sceneInputs = {};
}

TEST_F(RenderCoreTest, DistanceFieldSuspendDropsAnUnfinishedBuildAndFreesItsScratch)
{
    sceneInputs.vertices  = Buffer(sizeof(asset::Vertex));
    sceneInputs.indices   = Buffer();
    sceneInputs.nodes     = Buffer();
    sceneInputs.materials = Buffer();
    sceneInputs.uv        = Buffer();
    sceneInputs.shadowCasters.push_back({sg::AABB(Vec3(-1), Vec3(1)), 1});
    sceneInputs.triangleRanges.push_back({0, 100, GI_STATIC, Vec4(0)});
    EnableDistanceFieldTestDevice(device);
    sg::Scene source;
    source.GetAABB() = sg::AABB(Vec3(-1), Vec3(1));
    SceneData data{};
    data.pScene = &source;
    RenderScene scene(device, data);
    source.GetAABB() = sg::AABB(Vec3(-1), Vec3(1));
    DistanceFieldSceneSettings settings;
    settings.cellsPerAxis            = 4;
    settings.atlasTexelsPerAxis      = 8;
    settings.jobCapacity             = 1;
    settings.referenceCapacity       = 1;
    settings.deriveCapacities        = true;
    settings.scratchBudgetBytes      = 65539;
    rhi->info.deviceLocalMemoryBytes = 8ull * 1024 * 1024 * 1024;
    rhi->bufferAllocationAlignment   = 256;
    rhi->bufferAllocationPadding     = 0;
    DistanceFieldScene field(device, settings);
    {
        RenderGraph& graph = *device->GetCurrentFrameRDG();
        ASSERT_TRUE(graph.Begin());
        ASSERT_EQ(field.BuildRenderGraph(scene), DistanceFieldScene::State::Pending) << field.GetFailureReason();
        graph.AddComputePass(IntentPass("keepalive"));
        ASSERT_TRUE(graph.End()) << graph.GetResult().message;
        ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
        field.OnRenderGraphExecuted(RenderFrameResult::eSubmitted);
        device->NextFrame();
    }
    ASSERT_GT(field.GetScratchBytes(), 0u);
    // HW-only frames record no field work; the dropped build's scratch retires with its GPU work.
    for (uint32_t frame = 0; frame < 8 && field.GetScratchBytes() != 0; ++frame)
    {
        RenderGraph& graph = *device->GetCurrentFrameRDG();
        ASSERT_TRUE(graph.Begin());
        field.Suspend();
        graph.AddComputePass(IntentPass("keepalive"));
        ASSERT_TRUE(graph.End()) << graph.GetResult().message;
        ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
        device->NextFrame();
    }
    EXPECT_EQ(field.GetScratchBytes(), 0u);
    EXPECT_EQ(field.GetState(), DistanceFieldScene::State::Pending);
    {
        // Selecting Compute again restarts the dropped build.
        RenderGraph& graph = *device->GetCurrentFrameRDG();
        ASSERT_TRUE(graph.Begin());
        EXPECT_EQ(field.BuildRenderGraph(scene), DistanceFieldScene::State::Pending) << field.GetFailureReason();
        EXPECT_GT(field.GetScratchBytes(), 0u);
        graph.AddComputePass(IntentPass("keepalive"));
        ASSERT_TRUE(graph.End()) << graph.GetResult().message;
        ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
        field.OnRenderGraphExecuted(RenderFrameResult::eSubmitted);
        device->NextFrame();
    }
    field.Destroy();
    for (RHIBuffer* buffer :
         {sceneInputs.vertices, sceneInputs.indices, sceneInputs.nodes, sceneInputs.materials, sceneInputs.uv})
    {
        device->DestroyBuffer(buffer);
    }
    sceneInputs = {};
}

// One visibility-only frame: provider selection and bounded field construction, with the frame's callback.
bool BuildRayQueryFrame(RenderDevice* device, VoxelGIRenderer& gi)
{
    RenderGraph& graph = *device->GetCurrentFrameRDG();
    EXPECT_TRUE(graph.Begin());
    const bool ready = gi.BuildRayQueryGraph();
    graph.AddComputePass(IntentPass("keepalive"));
    EXPECT_TRUE(graph.End()) << graph.GetResult().message;
    EXPECT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
    gi.OnVisibilityGraphExecuted(RenderFrameResult::eSubmitted);
    device->NextFrame();
    return ready;
}

// Visibility-only frames until the scene's field is published and Compute GI traces it.
bool PrepareComputeField(RenderDevice* device, VoxelGIRenderer& gi)
{
    EnableDistanceFieldTestDevice(device);
    for (uint32_t frame = 0; frame < 32 && !gi.UsesFieldTraversal(); ++frame)
    {
        BuildRayQueryFrame(device, gi);
    }
    return gi.UsesFieldTraversal();
}

TEST_F(RenderCoreTest, VoxelGIFieldAtlasCanAddressEverySupportedCell)
{
    sceneInputs.vertices  = Buffer(sizeof(asset::Vertex));
    sceneInputs.indices   = Buffer();
    sceneInputs.nodes     = Buffer();
    sceneInputs.materials = Buffer();
    sceneInputs.uv        = Buffer();
    EnableDistanceFieldTestDevice(device);
    sg::Scene source;
    SceneData data{};
    data.pScene = &source;
    RenderScene      scene(device, data);
    TestVoxelVolumes volumes(device, DataFormat::eR8G8B8A8UNORM);
    volumes.Init();
    volumes.SetRenderScene(&scene);
    VoxelGIRenderer gi(device, &volumes);
    gi.SetRenderScene(&scene);
    ASSERT_TRUE(gi.Init());
    for (uint32_t side : {32u, 64u})
    {
        SCOPED_TRACE(side);
        VoxelGISettings settings;
        settings.fieldResolution = side;
        settings.rayProvider     = VoxelGISettings::RayProvider::Voxel;
        ASSERT_TRUE(gi.SetSettings(settings));
        BuildRayQueryFrame(device, gi);
        ASSERT_TRUE(PrepareComputeField(device, gi));
        const DistanceFieldScene* field = gi.GetDistanceField();
        ASSERT_NE(field, nullptr);
        const DistanceFieldSceneUniform& uniform = field->GetUniform();
        const uint64_t                   cells   = uint64_t(side) * side * side;
        const uint64_t                   bricks =
            uint64_t(uniform.atlasDimensions.x / 8) * (uniform.atlasDimensions.y / 8) * (uniform.atlasDimensions.z / 8);
        EXPECT_EQ(uniform.dimensions.x, side);
        EXPECT_GE(uniform.dimensions.y, cells);
        EXPECT_GE(bricks, cells);
        EXPECT_LE(field->GetPersistentBytes(), 256ull * 1024 * 1024);

        // A full-capacity replacement must fit beside the published field while geometry changes.
        const uint64_t generation = gi.GetVisibilityGeneration();
        const uint32_t queries    = rhi->bufferAllocationQueries;
        ASSERT_TRUE(scene.CommitGeometryUpdates());
        ASSERT_TRUE(BuildRayQueryFrame(device, gi));
        EXPECT_TRUE(gi.UsesFieldTraversal());
        EXPECT_EQ(gi.GetVisibilityGeneration(), generation);
        EXPECT_LE(field->GetPersistentBytes(), 512ull * 1024 * 1024);
        for (uint32_t frame = 0; frame < 8 && !field->IsCurrent(scene); ++frame)
        {
            ASSERT_TRUE(BuildRayQueryFrame(device, gi));
            EXPECT_TRUE(gi.UsesFieldTraversal());
        }
        EXPECT_TRUE(field->IsCurrent(scene));
        EXPECT_NE(gi.GetVisibilityGeneration(), generation);
        // The refresh needs the same native sizes, so it must not wait on the RHI thread again.
        EXPECT_EQ(rhi->bufferAllocationQueries, queries);
    }
    gi.Destroy();
    volumes.Destroy();
    for (RHIBuffer* buffer :
         {sceneInputs.vertices, sceneInputs.indices, sceneInputs.nodes, sceneInputs.materials, sceneInputs.uv})
    {
        device->DestroyBuffer(buffer);
    }
    sceneInputs = {};
}

// Capability fallback RT pipeline -> Compute -> PBR. An automatic HW tier that cannot build the scene
// moves to Compute and stays there; an explicit HW tier reports the failure and the frame uses PBR.
TEST_F(RenderCoreTest, AutomaticTierFallsBackFromHardwareToCompute)
{
    using Provider        = VoxelGISettings::RayProvider;
    sceneInputs.vertices  = Buffer(sizeof(asset::Vertex));
    sceneInputs.indices   = Buffer();
    sceneInputs.nodes     = Buffer();
    sceneInputs.materials = Buffer();
    sceneInputs.uv        = Buffer();
    EnableDistanceFieldTestDevice(device);
    rhi->info.rayQuery = {true, true, true, true, true, 256, 1024, 1024, 1024};
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
    VoxelGISettings settings;
    settings.rayProvider = Provider::Hardware;
    ASSERT_TRUE(gi.SetSettings(settings));
    gi.SetAutomaticTier(true);

    // The scene's acceleration-structure inputs fail to allocate: a persistent HW failure.
    rhi->failBufferCreationAt = rhi->bufferCreations + 2;
    EXPECT_FALSE(BuildRayQueryFrame(device, gi));
    EXPECT_STREQ(gi.GetTierFallbackReason(), "scene_build_preparation_failed");
    EXPECT_FALSE(gi.UsesHardwareQueries());
    EXPECT_EQ(gi.GetHardwareFeatureMask(), 0u);
    EXPECT_TRUE(gi.IsPreparingField());
    EXPECT_STREQ(gi.GetProviderReason(), "field_preparing");
    EXPECT_FALSE(gi.UsesHardwareShadows());
    bool ready = false;
    for (uint32_t frame = 0; frame < 16 && !ready; ++frame)
    {
        ready = BuildRayQueryFrame(device, gi);
    }
    ASSERT_TRUE(ready);
    EXPECT_TRUE(gi.UsesFieldTraversal());
    // The fallback is kept although HW could build now; it does not retry every frame.
    const size_t builds = rhi->graphics.accelerationStructureBuildCounts.size();
    ASSERT_TRUE(BuildRayQueryFrame(device, gi));
    EXPECT_FALSE(gi.UsesHardwareQueries());
    EXPECT_EQ(rhi->graphics.accelerationStructureBuildCounts.size(), builds);
    EXPECT_STREQ(gi.GetTierFallbackReason(), "scene_build_preparation_failed");

    // Choosing the HW tier explicitly retries it.
    gi.SetAutomaticTier(false);
    EXPECT_EQ(gi.GetTierFallbackReason(), nullptr);
    ASSERT_TRUE(BuildRayQueryFrame(device, gi));
    EXPECT_TRUE(gi.UsesHardwareQueries());
    EXPECT_FALSE(gi.UsesFieldTraversal());

    // An explicit HW tier that cannot build stays unavailable: PBR, with its reason, and no field.
    gi.SetRenderScene(&scene);
    rhi->failBufferCreationAt = rhi->bufferCreations + 2;
    EXPECT_FALSE(BuildRayQueryFrame(device, gi));
    EXPECT_EQ(gi.GetTierFallbackReason(), nullptr);
    EXPECT_STREQ(gi.GetProviderReason(), "scene_build_preparation_failed");
    EXPECT_FALSE(gi.UsesHardwareQueries());
    EXPECT_FALSE(gi.IsPreparingField());
    EXPECT_FALSE(gi.UsesFieldTraversal());
    // The next frame retries the explicit tier.
    ASSERT_TRUE(BuildRayQueryFrame(device, gi));
    EXPECT_TRUE(gi.UsesHardwareQueries());

    // A requested RT-pipeline tier without RT pipelines is a capability miss: Compute, no HW build.
    gi.SetAutomaticTier(true);
    settings.rayTracingPipeline = true;
    ASSERT_TRUE(gi.SetSettings(settings));
    rhi->info.rayTracing       = {};
    const size_t beforeCompute = rhi->graphics.accelerationStructureBuildCounts.size();
    EXPECT_FALSE(BuildRayQueryFrame(device, gi));
    EXPECT_FALSE(gi.UsesHardwareQueries());
    EXPECT_EQ(gi.GetHardwareFeatureMask(), 0u);
    EXPECT_TRUE(gi.IsPreparingField());
    EXPECT_EQ(gi.GetTierFallbackReason(), nullptr);
    EXPECT_EQ(rhi->graphics.accelerationStructureBuildCounts.size(), beforeCompute);
    // With capable RT pipelines, the same request selects the RT-pipeline HW tier.
    rhi->info.rayTracing = {true, false, RHIShaderInvocationReorder::eNone, 32, 32, 64, 4096, 2, 1048576, 32};
    ASSERT_TRUE(BuildRayQueryFrame(device, gi));
    EXPECT_TRUE(gi.UsesHardwareQueries());
    EXPECT_TRUE(gi.UsesRayTracingPipeline());
    rhi->info.rayTracing = {};
    gi.Destroy();
    volumes.Destroy();
    for (RHIBuffer* buffer :
         {sceneInputs.vertices, sceneInputs.indices, sceneInputs.nodes, sceneInputs.materials, sceneInputs.uv})
    {
        device->DestroyBuffer(buffer);
    }
    sceneInputs = {};
}

// One frame of a scene that changes every frame, as an animated one does: the facade's commit
// advances the geometry revision. Returns whether the frame's GI ray queries were ready.
bool BuildChangedRayQueryFrame(RenderDevice* device, RenderScene& scene, VoxelGIRenderer& gi)
{
    EXPECT_TRUE(scene.CommitGeometryUpdates());
    RenderGraph& graph = *device->GetCurrentFrameRDG();
    EXPECT_TRUE(graph.Begin());
    const bool ready = gi.BuildRayQueryGraph();
    graph.AddComputePass(IntentPass("keepalive"));
    EXPECT_TRUE(graph.End()) << graph.GetResult().message;
    EXPECT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
    gi.OnVisibilityGraphExecuted(RenderFrameResult::eSubmitted);
    gi.OnRenderGraphExecuted(RenderFrameResult::eSubmitted);
    device->NextFrame();
    return ready;
}

// Compute never builds or borrows HW RT, even on a capable device or immediately after HW use.
TEST_F(RenderCoreTest, ComputeFieldPreparationDoesNotBorrowHardware)
{
    using Provider        = VoxelGISettings::RayProvider;
    sceneInputs.vertices  = Buffer(sizeof(asset::Vertex));
    sceneInputs.indices   = Buffer();
    sceneInputs.nodes     = Buffer();
    sceneInputs.materials = Buffer();
    sceneInputs.uv        = Buffer();
    EnableDistanceFieldTestDevice(device);
    rhi->info.rayQuery = {true, true, true, true, true, 256, 1024, 1024, 1024};
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
    const uint32_t  allFeatures = 63u;
    VoxelGISettings settings;
    settings.rayProvider = Provider::Voxel;
    ASSERT_TRUE(gi.SetSettings(settings));

    // A capable device still prepares Compute without any acceleration-structure build.
    EXPECT_FALSE(BuildRayQueryFrame(device, gi));
    EXPECT_TRUE(gi.IsPreparingField());
    EXPECT_FALSE(gi.UsesFieldTraversal());
    EXPECT_EQ(gi.GetVisibilityGeneration(), 0u);
    EXPECT_STREQ(gi.GetProviderReason(), "field_preparing");
    EXPECT_FALSE(gi.UsesHardwareQueries());
    EXPECT_TRUE(rhi->graphics.accelerationStructureBuildCounts.empty());
    EXPECT_TRUE(rhi->compute.accelerationStructureBuildCounts.empty());
    bool ready = false;
    for (uint32_t frame = 0; frame < 8 && !ready; ++frame)
    {
        ready = BuildRayQueryFrame(device, gi);
    }
    ASSERT_TRUE(ready);
    ASSERT_TRUE(BuildRayQueryFrame(device, gi));
    EXPECT_TRUE(gi.UsesFieldTraversal());
    EXPECT_FALSE(gi.IsPreparingField());
    EXPECT_EQ(gi.GetHardwareFeatureMask(), 0u);
    const uint64_t visibility = gi.GetVisibilityGeneration();
    EXPECT_NE(visibility, 0u);

    // HW RT needs no field and keeps the published one; returning to Compute uses it at once.
    rhi->info.rayQuery   = {true, true, true, true, true, 256, 1024, 1024, 1024};
    settings.rayProvider = Provider::Hardware;
    ASSERT_TRUE(gi.SetSettings(settings));
    ASSERT_TRUE(BuildRayQueryFrame(device, gi));
    EXPECT_FALSE(gi.UsesFieldTraversal());
    EXPECT_EQ(gi.GetHardwareFeatureMask(), allFeatures);
    EXPECT_FALSE(gi.UsesVoxelLighting());
    EXPECT_EQ(gi.GetDistanceField()->GetState(), DistanceFieldScene::State::Ready);
    // A usable depth-one RT device cannot execute primary closest-hit shadow rays.
    settings.rayTracingPipeline = true;
    ASSERT_TRUE(gi.SetSettings(settings));
    rhi->info.rayTracing = {true, false, RHIShaderInvocationReorder::eNone, 32, 32, 64, 4096, 1, 1048576, 32};
    ASSERT_TRUE(rhi->info.rayTracing.IsUsable());
    EXPECT_FALSE(gi.UsesRayTracingPipeline());
    rhi->info.rayTracing.maxRayRecursionDepth = 2;
    EXPECT_TRUE(gi.UsesRayTracingPipeline());
    rhi->info.rayTracing.pipeline = false;
    EXPECT_FALSE(gi.UsesRayTracingPipeline());
    rhi->info.rayTracing.pipeline = true;
    settings.rayTracingPipeline   = false;
    ASSERT_TRUE(gi.SetSettings(settings));
    EXPECT_FALSE(gi.UsesRayTracingPipeline());
    rhi->info.rayTracing = {};
    const size_t builds  = rhi->graphics.accelerationStructureBuildCounts.size();
    settings.rayProvider = Provider::Voxel;
    ASSERT_TRUE(gi.SetSettings(settings));
    ASSERT_TRUE(BuildRayQueryFrame(device, gi));
    EXPECT_TRUE(gi.UsesFieldTraversal());
    EXPECT_FALSE(gi.UsesHardwareQueries());
    EXPECT_EQ(rhi->graphics.accelerationStructureBuildCounts.size(), builds);
    EXPECT_EQ(gi.GetVisibilityGeneration(), visibility);

    // A geometry change keeps the published field, with the edited regions stale, while the next
    // generation builds from a snapshot; publishing it restarts the visibility histories.
    ASSERT_TRUE(gi.SetSettings(settings));
    ASSERT_TRUE(BuildChangedRayQueryFrame(device, scene, gi));
    EXPECT_FALSE(gi.IsPreparingField());
    EXPECT_TRUE(gi.UsesFieldTraversal());
    EXPECT_EQ(gi.GetHardwareFeatureMask(), 0u);
    EXPECT_FALSE(gi.GetDistanceField()->IsCurrent(scene));
    EXPECT_EQ(gi.GetVisibilityGeneration(), visibility);
    for (uint32_t frame = 0; frame < 8 && !gi.GetDistanceField()->IsCurrent(scene); ++frame)
    {
        ASSERT_TRUE(BuildRayQueryFrame(device, gi));
        EXPECT_TRUE(gi.UsesFieldTraversal());
    }
    EXPECT_TRUE(gi.GetDistanceField()->IsCurrent(scene));
    EXPECT_NE(gi.GetVisibilityGeneration(), visibility);
    // Changing to a cold Compute field after HW use must not retain HW as a preview.
    settings.rayProvider     = Provider::Voxel;
    settings.fieldResolution = 32;
    ASSERT_TRUE(gi.SetSettings(settings));
    const size_t beforeCold = rhi->graphics.accelerationStructureBuildCounts.size();
    EXPECT_FALSE(BuildRayQueryFrame(device, gi));
    EXPECT_TRUE(gi.IsPreparingField());
    EXPECT_FALSE(gi.UsesHardwareQueries());
    EXPECT_EQ(gi.GetHardwareFeatureMask(), 0u);
    EXPECT_EQ(rhi->graphics.accelerationStructureBuildCounts.size(), beforeCold);
    // Explicit HW unavailability does not silently select the field either.
    settings.rayProvider = Provider::Hardware;
    rhi->info.rayQuery   = {};
    ASSERT_TRUE(gi.SetSettings(settings));
    EXPECT_FALSE(BuildRayQueryFrame(device, gi));
    EXPECT_FALSE(gi.UsesFieldTraversal());
    EXPECT_FALSE(gi.IsPreparingField());
    gi.Destroy();
    volumes.Destroy();
    for (RHIBuffer* buffer :
         {sceneInputs.vertices, sceneInputs.indices, sceneInputs.nodes, sceneInputs.materials, sceneInputs.uv})
    {
        device->DestroyBuffer(buffer);
    }
    sceneInputs = {};
}
