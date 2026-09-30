TEST_P(DynamicVoxelGIIntegrationTest, RuntimeSettingsReplaceInFlightVolumesAndPreserveLiveEdits)
{
    RendererServer server(device, nullptr);

    server.Init();

    VoxelGIRuntimeSettings settings = server.GetVoxelGISettings();

    settings.dynamic.method = VoxelGIMethod::eCone;

    settings.dynamic.memoryBudgetBytes = 1024ull * 1024 * 1024;

    settings.averagedReflectance = false;

    uint64_t previousOwner = 0;

    for (uint32_t iteration = 0; iteration < 4; ++iteration)
    {
        SCOPED_TRACE(iteration);

        settings.dynamic.resolution = iteration % 2 == 0 ? 64 : 128;

        settings.voxelizer = iteration % 2 == 0 ? platform::VoxelizerMode::eCompute :
                                                  platform::VoxelizerMode::eGeometry;

        settings.asyncCompute =
            iteration % 2 == 0 ? AsyncComputeMode::eDisabled : AsyncComputeMode::eAuto;

        settings.shadowMapResolution = iteration % 2 == 0 ? 256 : 512;

        EXPECT_TRUE(server.ApplyVoxelGISettings(settings));

        VoxelizerBase* voxelizer = server.RequestVoxelizer();

        EXPECT_EQ(voxelizer->GetVoxelTexResolution(), settings.dynamic.resolution);

        EXPECT_TRUE(server.RequestVoxelGI()->Init());

        RHITexture* owner = voxelizer->GetVoxelTextures().pOwner;

        EXPECT_NE(owner->GetStableId(), previousOwner);

        previousOwner = owner->GetStableId();

        EXPECT_EQ(owner->GetBaseInfo().width, settings.dynamic.resolution);

        RenderGraph& graph = *device->GetCurrentFrameRDG();

        EXPECT_TRUE(graph.Begin());

        graph.AddTransferPass("RuntimeOwnerClear").ClearTexture(owner, Color(0)).NeverCull();

        EXPECT_FALSE(server.ApplyVoxelGISettings(settings));

        EXPECT_FALSE(device->SetAsyncComputeMode(settings.asyncCompute));

        EXPECT_TRUE(graph.End());

        EXPECT_TRUE(device->ExecuteRenderGraph(graph));

        // No explicit flush/idle: the replacement API owns retirement of this submission.
        settings.cone.indirectIntensity = 2.0f;

        EXPECT_TRUE(server.ApplyVoxelGISettings(settings));

        EXPECT_EQ(server.RequestVoxelizer(), voxelizer);

        EXPECT_EQ(server.RequestVoxelGI()->GetSettings().indirectIntensity, 2.0f);

        VoxelGIRuntimeSettings invalid = settings;

        invalid.dynamic.resolution = 63;

        EXPECT_FALSE(server.ApplyVoxelGISettings(invalid));

        EXPECT_EQ(server.RequestVoxelizer(), voxelizer);

        EXPECT_EQ(server.GetVoxelGISettings().dynamic.resolution, settings.dynamic.resolution);
    }

    EXPECT_TRUE(device->PrepareForResourceReconfiguration());

    server.Destroy();

    device->CollectCompletedResources();
}

TEST_P(DynamicVoxelGIIntegrationTest, RuntimeSettingsRecreateDirectionalCachesAndReleaseBudget)
{
    sg::Scene source;

    source.GetAABB() = sg::AABB(Vec3(-1), Vec3(1));

    // A geometry-free scene still needs nonempty source buffers at the native API boundary.
    const asset::Vertex vertices[3]{};

    const uint32_t indices[3]{0, 1, 2};

    SceneData data{};

    data.pScene = &source;

    data.pVertices = vertices;

    data.pIndices = indices;

    data.numVertices = data.numIndices = 3;

    RenderScene scene(device, data);

    RendererServer server(device, nullptr);

    server.Init();

    server.SetRenderScene(&scene);

    VoxelGIRuntimeSettings settings = server.GetVoxelGISettings();

    settings.dynamic.resolution = 64;

    settings.dynamic.method = VoxelGIMethod::eDynamicVoxel;

    settings.dynamic.backend = VoxelGIQueryBackend::eVoxelDDA;

    settings.dynamic.memoryBudgetBytes = 512ull * 1024 * 1024;

    settings.averagedReflectance = false;

    uint64_t previousCache = 0;

    for (uint32_t iteration = 0; iteration < 3; ++iteration)
    {
        settings.dynamic.compactCache = iteration != 1;

        settings.dynamic.raysPerFace = iteration == 0 ? 32 : iteration == 1 ? 64 : 128;

        settings.dynamic.neighborRadius = iteration == 1 ? 1 : 2;

        settings.averagedReflectance = iteration == 2;

        settings.reflectanceBudgetBytes = 16ull * 1024 * 1024;

        EXPECT_TRUE(server.ApplyVoxelGISettings(settings));

        EXPECT_EQ(server.RequestDynamicVoxelGI(), nullptr);

        EXPECT_TRUE(VoxelGIRuntimeTestAccess::Prepare(server));

        DynamicVoxelGIRenderer* renderer = server.RequestDynamicVoxelGI();

        if (renderer != nullptr && renderer->GetResourceBytes() != 0)
        {
            EXPECT_EQ(renderer->GetRaysPerFace(), settings.dynamic.raysPerFace);

            EXPECT_EQ(renderer->GetCacheStride(), settings.dynamic.compactCache ? 8u : 96u);

            EXPECT_EQ(renderer->GetSettings().neighborRadius, settings.dynamic.neighborRadius);

            EXPECT_NE(renderer->GetCache(0)->GetStableId(), previousCache);

            previousCache = renderer->GetCache(0)->GetStableId();

            // Method switches and filtering-only edits preserve the allocated cache.
            settings.dynamic.temporal = GITemporalMode::eFixed;

            EXPECT_TRUE(server.ApplyVoxelGISettings(settings));

            EXPECT_EQ(server.RequestDynamicVoxelGI()->GetCache(0)->GetStableId(), previousCache);

            EXPECT_TRUE(server.SetVoxelGIMethod(VoxelGIMethod::eCone));

            EXPECT_FALSE(VoxelGIRuntimeTestAccess::Prepare(server));

            EXPECT_TRUE(server.SetVoxelGIMethod(VoxelGIMethod::eDynamicVoxel));

            EXPECT_TRUE(VoxelGIRuntimeTestAccess::Prepare(server));

            EXPECT_EQ(server.RequestDynamicVoxelGI()->GetCache(0)->GetStableId(), previousCache);
        }
    }

    settings.dynamic.backend = VoxelGIQueryBackend::eHardwareRT;

    EXPECT_TRUE(server.ApplyVoxelGISettings(settings));

    EXPECT_FALSE(VoxelGIRuntimeTestAccess::Prepare(server));

    EXPECT_EQ(server.GetVoxelGISelection().method, VoxelGIMethod::eCone);

    settings.dynamic.backend = VoxelGIQueryBackend::eVoxelDDA;

    settings.dynamic.memoryBudgetBytes = 1;

    EXPECT_TRUE(server.ApplyVoxelGISettings(settings));

    EXPECT_FALSE(VoxelGIRuntimeTestAccess::Prepare(server));

    EXPECT_EQ(server.GetVoxelGISelection().method, VoxelGIMethod::eNone);

    EXPECT_EQ(server.RequestDynamicVoxelGI(), nullptr);

    EXPECT_EQ(server.GetVoxelGISettings().dynamic.memoryBudgetBytes, 1u);

    EXPECT_TRUE(device->PrepareForResourceReconfiguration());

    server.Destroy();

    device->CollectCompletedResources();
}

TEST_P(DynamicVoxelGIIntegrationTest, RuntimeTemporalParametersResetHistoryAndReachGPU)
{
    RenderGraph& graph = *device->GetCurrentFrameRDG();

    DynamicVoxelGIRenderer* renderer = StaticRenderer(1);

    renderer->SetFiltering(GITemporalMode::eFixed, false);

    const FilterCapture capture = CreateFilterCapture();

    ASSERT_TRUE(graph.Begin());

    StaticFixture fixture = StaticInputs(graph, {{{10, 10, 10}, GI_STATIC}});

    const HeapVector<GIHit> responses = ConstantResponses(1, fixture.ids[0]);

    const DeterministicGIProvider provider = ReferenceStatic(graph, responses, 1);

    SceneUniformData lighting = StaticLighting();

    fixture.inputs.timeSeconds = 0;

    ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, true));

    const FilterReadback initial = CaptureFilterCell(graph, *renderer, capture, fixture.ids[0]);

    ASSERT_EQ(initial.status.z, 0);

    const uint64_t epoch = renderer->GetCacheEpoch();

    ASSERT_TRUE(renderer->SetTemporalParameters(0.5f, 0.6f, 30.0f));

    ASSERT_TRUE(renderer->SetCacheBatchSize(16));

    EXPECT_FALSE(renderer->SetTemporalParameters(2.0f, 0.6f, 30.0f));

    EXPECT_FALSE(renderer->SetCacheBatchSize(0));

    lighting.lights[0].colorIntensity.w = 0;

    fixture.inputs.timeSeconds = 0.01;

    ASSERT_TRUE(graph.Begin());

    ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, true));

    const FilterReadback reset = CaptureFilterCell(graph, *renderer, capture, fixture.ids[0]);

    EXPECT_EQ(reset.history, reset.raw);

    EXPECT_EQ(renderer->GetCacheEpoch(), epoch);

    EXPECT_EQ(Vec3(renderer->GetFilterUniform().timing.y, renderer->GetFilterUniform().timing.z,
                   renderer->GetFilterUniform().timing.w),
              Vec3(0.6f, 0.5f, 30.0f));

    lighting = StaticLighting();

    fixture.inputs.timeSeconds = 0.02;

    ASSERT_TRUE(graph.Begin());

    ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, true));

    const FilterReadback filtered = CaptureFilterCell(graph, *renderer, capture, fixture.ids[0]);

    EXPECT_NEAR(filtered.history.z, filtered.raw.z * 0.5f, 0.0001f);

    renderer->SetFiltering(GITemporalMode::eElapsed, false);

    ASSERT_TRUE(renderer->SetTemporalParameters(1.0f, 0.6f, 30.0f));

    ASSERT_TRUE(graph.Begin());

    ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, true));

    const FilterReadback instant = CaptureFilterCell(graph, *renderer, capture, fixture.ids[0]);

    lighting.lights[0].colorIntensity.w = 0;

    ASSERT_TRUE(graph.Begin());

    ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, true));

    const FilterReadback sameTime = CaptureFilterCell(graph, *renderer, capture, fixture.ids[0]);

    EXPECT_EQ(sameTime.history, instant.history);

    fixture.inputs.timeSeconds = 0.03;

    ASSERT_TRUE(graph.Begin());

    ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, true));

    const FilterReadback advanced = CaptureFilterCell(graph, *renderer, capture, fixture.ids[0]);

    EXPECT_EQ(advanced.history, advanced.raw);

    EXPECT_EQ(advanced.history.z, 0.0f);
}
