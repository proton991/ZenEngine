TEST_P(DynamicVoxelGIIntegrationTest, WideGridBoundaryTransportFilteringAndRemoval)
{
    constexpr uint32_t side = 128;
    const glm::uvec3 receiver(125, 125, 125), sender(125, 125, 127);
    const HeapVector<OccupiedCell> cells = {{receiver, GI_STATIC}, {sender, GI_DYNAMIC}};
    RenderGraph& graph                   = *device->GetCurrentFrameRDG();
    DynamicVoxelGIRenderer* renderer     = StaticRenderer(1, 2, side);
    renderer->SetLighting(false, true, true);
    const FilterCapture capture = CreateFilterCapture();
    ASSERT_TRUE(graph.Begin());
    StaticFixture fixture          = FrameInputs(graph, cells, 1, side);
    fixture.inputs.grid.averaged.y = 1;
    EnvironmentInputs(graph, fixture, Color(0.25f, 0.5f, 1, 1));
    fixture.inputs.timeSeconds = 0;
    VoxelDDAProvider provider;
    ASSERT_TRUE(
        provider.Prepare(fixture.staticVoxels, fixture.dynamicVoxels, fixture.inputs.grid, 1));
    SceneUniformData lighting = StaticLighting();
    lighting.lightInfo.x      = 0;
    lighting.environment      = Vec4(1, 0, 1, 0);
    ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, true));
    const FilterReadback raw =
        CaptureFilterCell(graph, *renderer, capture, StaticID(receiver, side), GI_STATIC, 4);
    ASSERT_EQ(raw.status.z, 0);
    const float fraction = FaceHitFraction(cells, receiver, GI_STATIC, 4, side);
    ASSERT_GT(fraction, 0);
    const Vec3 expected = glm::pi<float>() *
        (Vec3(0.25f, 0.5f, 1) * (Vec3(1 - fraction) + Vec3(32, 64, 128) / 255.0f * fraction) +
         Vec3(8, 0.5f, 0.125f) * fraction);
    for (uint32_t c = 0; c < 3; ++c)
    {
        EXPECT_NEAR(raw.raw[c], expected[c], 0.004f);
    }
    ASSERT_TRUE(graph.Begin());
    renderer->SetFiltering(GITemporalMode::eElapsed, true);
    fixture.inputs.timeSeconds = 1.0 / 60;
    ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, true));
    const FilterReadback filtered =
        CaptureFilterCell(graph, *renderer, capture, StaticID(sender, side), GI_DYNAMIC, 4);
    EXPECT_EQ(filtered.status.z, 0);
    EXPECT_EQ(filtered.filtered.w, 1);
    EXPECT_GT(filtered.filtered.z, 0);
    RHITextureView* sky = fixture.inputs.environment;
    RHISampler* sampler = fixture.inputs.environmentSampler;
    ASSERT_TRUE(graph.Begin());
    fixture                            = FrameInputs(graph, {{receiver, GI_STATIC}}, 1, side);
    fixture.inputs.grid.averaged.y     = 1;
    fixture.inputs.normal              = fixture.staticVoxels.pNormal;
    fixture.inputs.dynamicNormal       = fixture.dynamicVoxels.pNormal;
    fixture.inputs.environment         = sky;
    fixture.inputs.environmentSampler  = sampler;
    fixture.inputs.environmentRevision = 1;
    fixture.inputs.historyRevision     = 1;
    fixture.inputs.timeSeconds         = 2.0 / 60;
    ASSERT_TRUE(
        provider.Prepare(fixture.staticVoxels, fixture.dynamicVoxels, fixture.inputs.grid, 2));
    ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, true));
    const FilterReadback removed =
        CaptureFilterCell(graph, *renderer, capture, StaticID(receiver, side), GI_STATIC, 4);
    EXPECT_EQ(removed.status.z, 0);
    EXPECT_EQ(removed.status.w, 0);
    EXPECT_NEAR(removed.raw.z, glm::pi<float>(), 0.002f);
    EXPECT_EQ(renderer->GetCacheBuildBatches(), 1);
}

TEST_P(DynamicVoxelGIIntegrationTest, ReplacementBudgetRejectsBeforeWaitingForPreviousGIWork)
{
    RenderGraph& graph               = *device->GetCurrentFrameRDG();
    DynamicVoxelGIRenderer* previous = StaticRenderer(1);
    ASSERT_TRUE(graph.Begin());
    StaticFixture fixture = FrameInputs(graph, {{{10, 10, 10}, GI_STATIC}}, 1);
    VoxelDDAProvider provider;
    ASSERT_TRUE(
        provider.Prepare(fixture.staticVoxels, fixture.dynamicVoxels, fixture.inputs.grid, 1));
    ASSERT_TRUE(previous->BuildRenderGraph(fixture.inputs, provider, StaticLighting(), 1, false));
    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    previous->OnRenderGraphExecuted(true);
    // No flush or completion wait before planning the replacement. Count the
    // previous method's owned grids and hit records as still unavailable for reuse.
    const uint64_t retiring = uint64_t(StaticCells) * GI_FRAME_CELL_BYTES + GI_FRAME_FIXED_BYTES +
        (StaticCells / GI_QUERY_GROUP_SIZE) * 32 + GI_FACE_COUNT * GI_FACE_RAYS * sizeof(GIHit);
    uint64_t classes = 0;
    ASSERT_EQ(ValidateVoxelClassResources(128, true, UINT64_MAX, 0, device->GetGPUInfo(), classes),
              GIResourceStatus::eSuccess);
    constexpr uint64_t cells = 128ull * 128 * 128;
    DynamicVoxelGISettings settings;
    settings.resolution        = 128;
    settings.memoryBudgetBytes = classes + cells * GI_FRAME_CELL_BYTES + GI_FRAME_FIXED_BYTES +
        (cells / GI_QUERY_GROUP_SIZE) * 32 + GI_FACE_COUNT * GI_FACE_RAYS * sizeof(GIHit);
    DynamicVoxelGIRenderer replacement(device);
    EXPECT_FALSE(replacement.Init(settings, true, 0, retiring));
    EXPECT_EQ(replacement.GetStatus(), nullptr);
    EXPECT_EQ(replacement.GetRawIrradiance(0), nullptr);
    ASSERT_TRUE(graph.Begin());
    const FilterCapture capture   = CreateFilterCapture();
    const FilterReadback retained = CaptureFilterCell(graph, *previous, capture, fixture.ids[0]);
    EXPECT_EQ(retained.status.z, 0);
    EXPECT_EQ(retained.raw.w, 1);
    replacement.Destroy();
}

TEST_P(DynamicVoxelGIIntegrationTest, QuerySettingsInvalidateCachedVisibilityWithoutGeometryChanges)
{
    RenderGraph& graph               = *device->GetCurrentFrameRDG();
    DynamicVoxelGIRenderer* renderer = StaticRenderer(1);
    const FilterCapture capture      = CreateFilterCapture();
    ASSERT_TRUE(graph.Begin());
    const StaticFixture fixture = StaticInputs(graph, {{{10, 10, 10}, GI_STATIC}});
    const glm::uvec2 settings[] = {{GI_ALL, 0}, {GI_DYNAMIC, 0}, {GI_ALL, 1}, {GI_ALL, 0}};
    VoxelDDAProvider provider;
    for (uint32_t step = 0; step < 4; ++step)
    {
        if (step != 0)
        {
            ASSERT_TRUE(graph.Begin());
        }
        GIGridUniform grid = fixture.inputs.grid;
        grid.dimensions.y  = settings[step].x;
        grid.dimensions.z  = settings[step].y;
        ASSERT_TRUE(provider.Prepare(fixture.staticVoxels, fixture.dynamicVoxels, grid, step + 1));
        ASSERT_TRUE(
            renderer->BuildRenderGraph(fixture.inputs, provider, StaticLighting(), 1, false));
        const FilterReadback value = CaptureFilterCell(graph, *renderer, capture, fixture.ids[0]);
        const bool known           = step == 0 || step == 3;
        EXPECT_EQ(value.status.z, known ? 0 : GI_STATIC_UNKNOWN);
        EXPECT_EQ(value.raw.w, known ? 1 : 0);
        EXPECT_EQ(renderer->GetCacheBuildBatches(), step + 1);
    }
}

TEST_P(DynamicVoxelGIIntegrationTest, CachedGeometryRefreshesSenderMaterialsWithoutRetracing)
{
    RenderGraph& graph               = *device->GetCurrentFrameRDG();
    DynamicVoxelGIRenderer* renderer = StaticRenderer(2);
    renderer->SetLighting(false, false, true);
    ASSERT_TRUE(graph.Begin());
    StaticFixture fixture =
        FrameInputs(graph, {{{10, 10, 10}, GI_STATIC}, {{10, 10, 12}, GI_STATIC}}, 1);
    VoxelDDAProvider provider;
    ASSERT_TRUE(
        provider.Prepare(fixture.staticVoxels, fixture.dynamicVoxels, fixture.inputs.grid, 1));
    SceneUniformData lighting = StaticLighting();
    lighting.lightInfo.x      = 0;
    ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, false));
    const StaticReadback initial = CaptureStatic(graph, *renderer);
    ASSERT_EQ(initial.status.z, 0);
    const uint32_t face = 2 * (4 * StaticCells + StaticID({10, 10, 10}));
    ASSERT_GT(initial.faces[face].r, 0);
    ASSERT_TRUE(graph.Begin());
    graph.AddTransferPass("ChangeSenderEmission")
        .ClearTexture(fixture.staticVoxels.pEmissive, Color(16, 1, 0.25f, 0));
    fixture.inputs.historyRevision = 1;
    ASSERT_TRUE(
        provider.Prepare(fixture.staticVoxels, fixture.dynamicVoxels, fixture.inputs.grid, 2));
    ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, false));
    const StaticReadback changed = CaptureStatic(graph, *renderer);
    EXPECT_EQ(changed.status.z, 0);
    EXPECT_EQ(renderer->GetCacheBuildBatches(), 1);
    ASSERT_EQ(initial.hits.size(), changed.hits.size());
    EXPECT_EQ(
        std::memcmp(initial.hits.data(), changed.hits.data(), initial.hits.size() * sizeof(GIHit)),
        0);
    for (uint32_t channel = 0; channel < 3; ++channel)
    {
        EXPECT_NEAR(changed.faces[face][channel], 2 * initial.faces[face][channel], 0.004f);
    }
}
