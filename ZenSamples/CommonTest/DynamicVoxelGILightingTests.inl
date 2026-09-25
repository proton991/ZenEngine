TEST_P(DynamicVoxelGIIntegrationTest, LightMasksReuseRadiometryAndCameraChangesIncludingBlackLights)
{
    RenderGraph& graph                 = *device->GetCurrentFrameRDG();
    DynamicVoxelGIRenderer* cached     = StaticRenderer(1);
    DynamicVoxelGIRenderer* recomputed = StaticRenderer(1);
    ASSERT_TRUE(graph.Begin());
    StaticFixture fixture =
        FrameInputs(graph, {{{10, 10, 10}, GI_STATIC}, {{10, 10, 12}, GI_DYNAMIC}}, 1);
    VoxelDDAProvider provider;
    ASSERT_TRUE(
        provider.Prepare(fixture.staticVoxels, fixture.dynamicVoxels, fixture.inputs.grid, 1));
    SceneUniformData lighting         = StaticLighting();
    lighting.lights[0].colorIntensity = Vec4(0);
    const LightMaskReadback dark =
        CheckCachedLightMasks(graph, *cached, *recomputed, fixture, provider, lighting, UINT32_MAX);
    EXPECT_EQ(dark.irradiance.status.z, 0);
    EXPECT_EQ(dark.irradiance.raw, Vec4(0, 0, 0, 1));
    // Even a cold black light has a geometric mask ready for becoming visible.
    EXPECT_EQ(dark.masks[StaticCells + StaticID({10, 10, 12})], 1);
    const Vec4 colors[] = {Vec4(1, 1, 1, 1), Vec4(0.25f, 0.5f, 1, 0.5f), Vec4(1, 1, 1, 0),
                           Vec4(0, 0, 0, 1), Vec4(1, 1, 1, 2)};
    for (const Vec4& color : colors)
    {
        ASSERT_TRUE(graph.Begin());
        lighting.lights[0].colorIntensity = color;
        ++fixture.inputs.historyRevision;
        const LightMaskReadback result =
            CheckCachedLightMasks(graph, *cached, *recomputed, fixture, provider, lighting, 0);
        EXPECT_EQ(result.irradiance.status.z, 0);
        EXPECT_TRUE(std::equal(result.masks.begin(), result.masks.end(), dark.masks.begin(),
                               dark.masks.end()));
        EXPECT_EQ(result.irradiance.raw.z > 0, color.z * color.w > 0);
    }
    ASSERT_TRUE(graph.Begin());
    ReceiverSurface(graph, fixture.inputs, Vec3(10.5f), GI_STATIC);
    lighting.viewPos = Vec4(3, 4, 5, 1);
    cached->SetFiltering(GITemporalMode::eOff, true);
    CheckCachedLightMasks(graph, *cached, *recomputed, fixture, provider, lighting, 0);
    EXPECT_EQ(cached->GetCacheBuildBatches(), 1);
}

TEST_P(DynamicVoxelGIIntegrationTest, LightMasksUpdateOnlyChangedSlotsAndRecoverRejectedPublication)
{
    RenderGraph& graph                 = *device->GetCurrentFrameRDG();
    DynamicVoxelGIRenderer* cached     = StaticRenderer(1);
    DynamicVoxelGIRenderer* recomputed = StaticRenderer(1);
    ASSERT_TRUE(graph.Begin());
    const StaticFixture fixture =
        FrameInputs(graph, {{{10, 10, 10}, GI_STATIC}, {{10, 10, 12}, GI_DYNAMIC}}, 1);
    VoxelDDAProvider provider;
    ASSERT_TRUE(
        provider.Prepare(fixture.staticVoxels, fixture.dynamicVoxels, fixture.inputs.grid, 1));
    SceneUniformData lighting = StaticLighting();
    lighting.lightInfo.x      = MaxSceneLights;
    for (uint32_t slot = 0; slot < MaxSceneLights; ++slot)
    {
        lighting.lights[slot]                 = lighting.lights[0];
        lighting.lights[slot].directionType.z = slot % 2 == 0 ? -1.0f : 1.0f;
    }
    CheckCachedLightMasks(graph, *cached, *recomputed, fixture, provider, lighting, UINT32_MAX);
    for (uint32_t step = 0; step < 12; ++step)
    {
        ASSERT_TRUE(graph.Begin());
        uint32_t updates = 1u << 4;
        switch (step)
        {
            case 0:
                lighting.lights[4].directionType.w = 1;
                lighting.lights[4].positionRange   = Vec4(10.5f, 10.5f, 15, 64);
                break;
            case 1: lighting.lights[4].positionRange.x += 5; break;
            case 2: lighting.lights[4].positionRange.w = 1; break;
            case 3:
                lighting.lights[4].positionRange.w = 64;
                lighting.lights[4].directionType.w = 2;
                lighting.lights[4].coneShadow      = Vec4(0.95f, 0.8f, 1, 0);
                break;
            case 4: lighting.lights[4].coneShadow.x = 0.9f; break;
            case 5: lighting.lights[4].coneShadow.z = 0; break;
            case 6:
                lighting.lights[31].directionType.z *= -1;
                updates = 1u << 31;
                break;
            case 7:
                lighting.lightInfo.x = 31;
                updates              = 1u << 31;
                break;
            case 8:
                lighting.lightInfo.x = 32;
                updates              = 1u << 31;
                break;
            case 9:
                std::swap(lighting.lights[3], lighting.lights[4]);
                updates |= 1u << 3;
                break;
            case 10:
                lighting.lightInfo.x = 0;
                updates              = UINT32_MAX;
                break;
            case 11:
                lighting.lightInfo.x = 1;
                updates              = 1;
                break;
        }
        const LightMaskReadback value = CheckCachedLightMasks(graph, *cached, *recomputed, fixture,
                                                              provider, lighting, updates);
        EXPECT_EQ(value.irradiance.status.z, 0);
    }
    for (bool shadows : {false, true})
    {
        ASSERT_TRUE(graph.Begin());
        CheckCachedLightMasks(graph, *cached, *recomputed, fixture, provider, lighting, UINT32_MAX,
                              shadows);
    }
    for (bool analytic : {false, true})
    {
        ASSERT_TRUE(graph.Begin());
        cached->SetLighting(analytic, false, false);
        recomputed->SetLighting(analytic, false, false);
        CheckCachedLightMasks(graph, *cached, *recomputed, fixture, provider, lighting, UINT32_MAX);
    }
    // Record a different light snapshot, reject the graph, then retry that snapshot.
    ASSERT_TRUE(graph.Begin());
    lighting.lights[0].directionType.z *= -1;
    ASSERT_TRUE(cached->BuildRenderGraph(fixture.inputs, provider, lighting, 1, true));
    EXPECT_EQ(cached->GetLightMaskUpdateBits(), 1);
    ASSERT_TRUE(graph.End());
    cached->OnRenderGraphExecuted(false);
    ASSERT_TRUE(graph.Begin());
    CheckCachedLightMasks(graph, *cached, *recomputed, fixture, provider, lighting, UINT32_MAX);
}

TEST_P(DynamicVoxelGIIntegrationTest, LightMasksInvalidateForOccludersGridAndQuerySettings)
{
    RenderGraph& graph                 = *device->GetCurrentFrameRDG();
    DynamicVoxelGIRenderer* cached     = StaticRenderer(1);
    DynamicVoxelGIRenderer* recomputed = StaticRenderer(1);
    VoxelDDAProvider provider;
    const SceneUniformData lighting = StaticLighting();
    for (uint32_t step = 0; step < 4; ++step)
    {
        ASSERT_TRUE(graph.Begin());
        HeapVector<OccupiedCell> cells = {{{10, 10, 10}, GI_STATIC}};
        if (step != 2)
        {
            cells.push_back(
                {step == 1 ? glm::uvec3(12, 10, 12) : glm::uvec3(10, 10, 12), GI_DYNAMIC});
        }
        const StaticFixture fixture = FrameInputs(graph, cells, 1);
        ASSERT_TRUE(provider.Prepare(fixture.staticVoxels, fixture.dynamicVoxels,
                                     fixture.inputs.grid, step + 1));
        const LightMaskReadback value = CheckCachedLightMasks(graph, *cached, *recomputed, fixture,
                                                              provider, lighting, UINT32_MAX);
        EXPECT_EQ(value.irradiance.status.z, 0);
        EXPECT_EQ(value.masks[StaticID({10, 10, 10})], step == 1 || step == 2 ? 1u : 0u);
        // Query controls and grid origin matter even without a generation change.
        ASSERT_TRUE(graph.Begin());
        GIGridUniform grid = fixture.inputs.grid;
        grid.dimensions.z  = 1;
        ASSERT_TRUE(provider.Prepare(fixture.staticVoxels, fixture.dynamicVoxels, grid, step + 1));
        CheckCachedLightMasks(graph, *cached, *recomputed, fixture, provider, lighting, UINT32_MAX);
        ASSERT_TRUE(graph.Begin());
        grid = fixture.inputs.grid;
        grid.minimumCellSize.x += 1;
        ASSERT_TRUE(provider.Prepare(fixture.staticVoxels, fixture.dynamicVoxels, grid, step + 1));
        CheckCachedLightMasks(graph, *cached, *recomputed, fixture, provider, lighting, UINT32_MAX);
    }
}

TEST_P(DynamicVoxelGIIntegrationTest, LightMasksPreserveUnknownUntilItsLightOrProviderIsRefreshed)
{
    RenderGraph& graph                 = *device->GetCurrentFrameRDG();
    DynamicVoxelGIRenderer* cached     = StaticRenderer(1);
    DynamicVoxelGIRenderer* recomputed = StaticRenderer(1);
    ASSERT_TRUE(graph.Begin());
    const StaticFixture fixture = StaticInputs(graph, {{{10, 10, 10}, GI_STATIC}});
    HeapVector<GIHit> responses = ConstantResponses(1, fixture.ids[0]);
    responses[GI_FACE_COUNT * GI_FACE_RAYS].identity.x = GI_UNKNOWN;
    DeterministicGIProvider provider                   = ReferenceStatic(graph, responses, 1);
    SceneUniformData lighting                          = StaticLighting();
    for (uint32_t step = 0; step < 4; ++step)
    {
        if (step != 0)
        {
            ASSERT_TRUE(graph.Begin());
        }
        uint32_t updates = step == 0 ? UINT32_MAX : 0;
        if (step == 1)
        {
            lighting.lights[0].colorIntensity.w *= 0.5f;
        }
        else if (step == 2)
        {
            lighting.lightInfo.x            = 2;
            lighting.lights[1]              = lighting.lights[0];
            lighting.lights[1].coneShadow.z = 0;
            updates                         = 2;
        }
        else if (step == 3)
        {
            lighting.lights[0].coneShadow.z = 0;
            updates                         = 1;
        }
        const LightMaskReadback value = CheckCachedLightMasks(graph, *cached, *recomputed, fixture,
                                                              provider, lighting, updates);
        EXPECT_EQ(value.irradiance.status.z, step == 3 ? 0u : GI_STATIC_UNKNOWN);
    }
    ASSERT_TRUE(graph.Begin());
    responses[GI_FACE_COUNT * GI_FACE_RAYS].identity.x = GI_MISS;
    provider                                           = ReferenceStatic(graph, responses, 2);
    lighting.lightInfo.x                               = 1;
    lighting.lights[0].coneShadow.z                    = 1;
    const LightMaskReadback recovered =
        CheckCachedLightMasks(graph, *cached, *recomputed, fixture, provider, lighting, UINT32_MAX);
    EXPECT_EQ(recovered.irradiance.status.z, 0);
}

TEST_P(DynamicVoxelGIIntegrationTest, AllLightBitsClearOnRemovalAndAnimationKeepsHistory)
{
    RenderGraph& graph               = *device->GetCurrentFrameRDG();
    DynamicVoxelGIRenderer* renderer = StaticRenderer(1);
    renderer->SetLighting(true, false, false);
    renderer->SetFiltering(GITemporalMode::eElapsed, false);
    const FilterCapture capture = CreateFilterCapture();
    ASSERT_TRUE(graph.Begin());
    StaticFixture fixture       = StaticInputs(graph, {{{10, 10, 10}, GI_STATIC}});
    HeapVector<GIHit> responses = ConstantResponses(1, fixture.ids[0]);
    for (GIHit& hit : responses)
    {
        hit.positionDistance = Vec4(10.5f, 10.5f, 10.5f, 1);
    }
    const DeterministicGIProvider provider = ReferenceStatic(graph, responses, 1);
    SceneUniformData lighting              = StaticLighting();
    for (uint32_t i = 0; i < MaxSceneLights; ++i)
    {
        lighting.lights[i]                = lighting.lights[0];
        lighting.lights[i].colorIntensity = Vec4(1, 1, 1, 1.0f / MaxSceneLights);
    }
    bool first = true;
    for (uint32_t count : {32u, 5u, 0u, 1u})
    {
        if (!first)
        {
            ASSERT_TRUE(graph.Begin());
        }
        first                = false;
        lighting.lightInfo.x = float(count);
        fixture.inputs.timeSeconds += 1.0 / 60;
        if (fixture.inputs.timeSeconds < 0)
        {
            fixture.inputs.timeSeconds = 0;
        }
        ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, false));
        const FilterReadback value = CaptureFilterCell(graph, *renderer, capture, fixture.ids[0]);
        ASSERT_EQ(value.status.z, 0);
        EXPECT_NEAR(value.raw.z, float(count) / MaxSceneLights, 0.0001f);
        EXPECT_EQ(value.raw, value.history); // Snapshot-size changes reset accumulated lighting.
        ASSERT_TRUE(graph.Begin());
        EXPECT_EQ(ReadLightMask(graph, *renderer, fixture.ids[0]),
                  count == 32 ? UINT32_MAX : (1u << count) - 1);
    }
    ASSERT_TRUE(graph.Begin());
    fixture.inputs.timeSeconds += 1.0 / 60;
    lighting.lights[0].colorIntensity.w = 1;
    ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, false));
    const FilterReadback animated = CaptureFilterCell(graph, *renderer, capture, fixture.ids[0]);
    EXPECT_NEAR(animated.history.z, 1.0f / 32 * 0.97f + 0.03f, 1e-5f);
    EXPECT_EQ(renderer->GetFilterUniform().control.z, 0);
}

TEST_P(DynamicVoxelGIIntegrationTest, ShadowMasksUseEveryEnabledLightAndBothOccupiedClasses)
{
    RenderGraph& graph               = *device->GetCurrentFrameRDG();
    DynamicVoxelGIRenderer* renderer = StaticRenderer(1);
    renderer->SetLighting(true, false, false);
    ASSERT_TRUE(graph.Begin());
    StaticFixture fixture =
        FrameInputs(graph, {{{10, 10, 10}, GI_STATIC}, {{10, 10, 12}, GI_DYNAMIC}}, 1);
    VoxelDDAProvider provider;
    ASSERT_TRUE(
        provider.Prepare(fixture.staticVoxels, fixture.dynamicVoxels, fixture.inputs.grid, 1));
    SceneUniformData lighting = StaticLighting();
    lighting.lightInfo.x      = 32;
    for (uint32_t i = 0; i < MaxSceneLights; ++i)
    {
        lighting.lights[i]                 = lighting.lights[0];
        lighting.lights[i].directionType.z = i % 2 == 0 ? -1.0f : 1.0f;
    }
    ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, true));
    EXPECT_EQ(ReadLightMask(graph, *renderer, StaticID({10, 10, 10})), 0xaaaaaaaau);
    ASSERT_TRUE(graph.Begin());
    EXPECT_EQ(ReadLightMask(graph, *renderer, StaticID({10, 10, 12}), GI_DYNAMIC), 0x55555555u);
    ASSERT_TRUE(graph.Begin());
    ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, false));
    EXPECT_EQ(ReadLightMask(graph, *renderer, StaticID({10, 10, 10})), UINT32_MAX);
}

TEST_P(DynamicVoxelGIIntegrationTest, EmissiveHitsWorkWithoutAnalyticLightForEveryClassPair)
{
    RenderGraph& graph               = *device->GetCurrentFrameRDG();
    DynamicVoxelGIRenderer* renderer = StaticRenderer(2);
    renderer->SetLighting(false, false, true);
    const FilterCapture capture = CreateFilterCapture();
    uint64_t generation         = 1;
    for (uint32_t receiverClass : {GI_STATIC, GI_DYNAMIC})
    {
        for (uint32_t senderClass : {GI_STATIC, GI_DYNAMIC})
        {
            ASSERT_TRUE(graph.Begin());
            const HeapVector<OccupiedCell> cells = {{{10, 10, 10}, receiverClass},
                                                    {{10, 10, 12}, senderClass}};
            StaticFixture fixture                = FrameInputs(graph, cells, generation);
            VoxelDDAProvider provider;
            ASSERT_TRUE(provider.Prepare(fixture.staticVoxels, fixture.dynamicVoxels,
                                         fixture.inputs.grid, generation++));
            SceneUniformData lighting = StaticLighting();
            lighting.lightInfo.x      = 0;
            ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, true));
            const FilterReadback value = CaptureFilterCell(
                graph, *renderer, capture, StaticID({10, 10, 10}), receiverClass, 4);
            ASSERT_EQ(value.status.z, 0);
            const float fraction = FaceHitFraction(cells, {10, 10, 10}, receiverClass, 4);
            ASSERT_GT(fraction, 0);
            const Vec3 expected = Vec3(8, 0.5f, 0.125f) * (glm::pi<float>() * fraction);
            for (uint32_t c = 0; c < 3; ++c)
            {
                EXPECT_NEAR(value.raw[c], expected[c], 0.004f);
            }
            ASSERT_TRUE(graph.Begin());
            EXPECT_EQ(ReadLightMask(graph, *renderer, StaticID({10, 10, 12}), senderClass), 0);
            ASSERT_TRUE(graph.Begin());
            ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 0, true));
            EXPECT_EQ(CaptureFilterCell(graph, *renderer, capture, StaticID({10, 10, 10}),
                                        receiverClass, 4)
                          .raw,
                      Vec4(0, 0, 0, 1));
        }
    }
}

TEST_P(DynamicVoxelGIIntegrationTest, EnvironmentSeparatesVisibleSkyFromReflectedSky)
{
    RenderGraph& graph               = *device->GetCurrentFrameRDG();
    DynamicVoxelGIRenderer* renderer = StaticRenderer(2);
    renderer->SetLighting(false, true, false);
    const FilterCapture capture = CreateFilterCapture();
    ASSERT_TRUE(graph.Begin());
    const HeapVector<OccupiedCell> cells = {{{10, 10, 10}, GI_STATIC}, {{10, 10, 12}, GI_STATIC}};
    StaticFixture fixture                = FrameInputs(graph, cells, 1);
    EnvironmentInputs(graph, fixture, Color(0.25f, 0.5f, 1, 1));
    VoxelDDAProvider provider;
    ASSERT_TRUE(
        provider.Prepare(fixture.staticVoxels, fixture.dynamicVoxels, fixture.inputs.grid, 1));
    SceneUniformData lighting = StaticLighting();
    lighting.lightInfo.x      = 0;
    lighting.environment      = Vec4(1, 0, 1, 0);
    const float fraction      = FaceHitFraction(cells, {10, 10, 10}, GI_STATIC, 4);
    bool first                = true;
    for (float gain : {0.0f, 1.0f, 2.0f})
    {
        if (!first)
        {
            ASSERT_TRUE(graph.Begin());
        }
        first = false;
        ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, gain, true));
        const FilterReadback value =
            CaptureFilterCell(graph, *renderer, capture, StaticID({10, 10, 10}), GI_STATIC, 4);
        ASSERT_EQ(value.status.z, 0);
        const Vec3 expected = Vec3(0.25f, 0.5f, 1) * glm::pi<float>() *
            (Vec3(1 - fraction) + Vec3(32, 64, 128) / 255.0f * fraction * gain);
        for (uint32_t c = 0; c < 3; ++c)
        {
            EXPECT_NEAR(value.raw[c], expected[c], 0.003f);
        }
        EXPECT_GT(value.raw.z, 0); // Visible sky remains at zero indirect gain.
    }
    ASSERT_TRUE(graph.Begin());
    const Vec4 sender = ReadSenderEnvironment(graph, *renderer, StaticID({10, 10, 12}), GI_STATIC);
    EXPECT_EQ(sender.w, 1);
    EXPECT_NEAR(sender.z, glm::pi<float>(), 0.002f);
    ASSERT_TRUE(graph.Begin());
    renderer->SetLighting(false, false, false);
    ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, true));
    EXPECT_EQ(
        CaptureFilterCell(graph, *renderer, capture, StaticID({10, 10, 10}), GI_STATIC, 4).raw,
        Vec4(0, 0, 0, 1));
}

TEST_P(DynamicVoxelGIIntegrationTest, SenderEnvironmentRefreshesForDynamicOccluderChanges)
{
    RenderGraph& graph               = *device->GetCurrentFrameRDG();
    DynamicVoxelGIRenderer* renderer = StaticRenderer(1);
    renderer->SetLighting(false, true, false);
    ASSERT_TRUE(graph.Begin());
    StaticFixture fixture =
        FrameInputs(graph, {{{10, 10, 10}, GI_STATIC}, {{10, 10, 11}, GI_DYNAMIC}}, 1);
    EnvironmentInputs(graph, fixture, Color(1, 1, 1, 1));
    VoxelDDAProvider provider;
    ASSERT_TRUE(
        provider.Prepare(fixture.staticVoxels, fixture.dynamicVoxels, fixture.inputs.grid, 1));
    SceneUniformData lighting = StaticLighting();
    lighting.lightInfo.x      = 0;
    lighting.environment      = Vec4(1, 0, 1, 0);
    ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, true));
    const Vec4 blocked = ReadSenderEnvironment(graph, *renderer, StaticID({10, 10, 10}), GI_STATIC);
    ASSERT_EQ(blocked.w, 1);
    EXPECT_LT(blocked.z, glm::pi<float>() * 0.8f);
    RHITextureView* sky = fixture.inputs.environment;
    RHISampler* sampler = fixture.inputs.environmentSampler;
    ASSERT_TRUE(graph.Begin());
    fixture                            = FrameInputs(graph, {{{10, 10, 10}, GI_STATIC}}, 1);
    fixture.inputs.normal              = fixture.staticVoxels.pNormal;
    fixture.inputs.dynamicNormal       = fixture.dynamicVoxels.pNormal;
    fixture.inputs.environment         = sky;
    fixture.inputs.environmentSampler  = sampler;
    fixture.inputs.environmentRevision = 1;
    ASSERT_TRUE(
        provider.Prepare(fixture.staticVoxels, fixture.dynamicVoxels, fixture.inputs.grid, 2));
    ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, true));
    const Vec4 unblocked =
        ReadSenderEnvironment(graph, *renderer, StaticID({10, 10, 10}), GI_STATIC);
    EXPECT_EQ(unblocked.w, 1);
    EXPECT_NEAR(unblocked.z, glm::pi<float>(), 0.002f);
    ASSERT_TRUE(graph.Begin());
    lighting.environment.x = 0.5f;
    ++fixture.inputs.environmentRevision;
    ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, true));
    const Vec4 dimmed = ReadSenderEnvironment(graph, *renderer, StaticID({10, 10, 10}), GI_STATIC);
    EXPECT_NEAR(dimmed.z, unblocked.z * 0.5f, 0.002f);
    // An incomplete traversal must never expose the environment through an unknown result.
    ASSERT_TRUE(graph.Begin());
    GIGridUniform truncated = fixture.inputs.grid;
    truncated.dimensions.z  = 1;
    ASSERT_TRUE(provider.Prepare(fixture.staticVoxels, fixture.dynamicVoxels, truncated, 3));
    ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, true));
    const FilterCapture capture = CreateFilterCapture();
    const FilterReadback unknown =
        CaptureFilterCell(graph, *renderer, capture, StaticID({10, 10, 10}));
    EXPECT_NE(unknown.status.z & GI_STATIC_UNKNOWN, 0);
    EXPECT_EQ(unknown.history, Vec4(0));
}

TEST_P(DynamicVoxelGIIntegrationTest, EnvironmentRotationUsesSourceCubeOrientation)
{
    RenderGraph& graph               = *device->GetCurrentFrameRDG();
    DynamicVoxelGIRenderer* renderer = StaticRenderer(1);
    renderer->SetLighting(false, true, false);
    const FilterCapture capture = CreateFilterCapture();
    ASSERT_TRUE(graph.Begin());
    StaticFixture fixture = FrameInputs(graph, {{{10, 10, 10}, GI_STATIC}}, 1);
    EnvironmentInputs(graph, fixture, Color(0, 0, 0, 1));
    const Vec4 colors[] = {Vec4(1, 0, 0, 1), Vec4(0, 1, 0, 1), Vec4(0, 0, 1, 1),
                           Vec4(1, 1, 0, 1), Vec4(1, 0, 1, 1), Vec4(0, 1, 1, 1)};
    RHIBuffer* upload   = Buffer(sizeof(colors), RHIBufferAllocateType::eCPUWrite, colors);
    graph.GetResourceManager()->ImportHostWrittenBuffer(upload);
    RHIBufferTextureCopyRegion region;
    region.textureSize = Vec3i(1);
    region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    region.textureSubresources.layerCount = 6;
    graph.AddTransferPass("ColoredEnvironmentFaces")
        .CopyBufferToTexture(upload, fixture.inputs.environment->GetTexture(), region);
    VoxelDDAProvider provider;
    ASSERT_TRUE(
        provider.Prepare(fixture.staticVoxels, fixture.dynamicVoxels, fixture.inputs.grid, 1));
    SceneUniformData lighting = StaticLighting();
    lighting.lightInfo.x      = 0;
    bool first                = true;
    for (float rotation : {0.0f, glm::half_pi<float>()})
    {
        for (uint32_t face = 0; face < GI_FACE_COUNT; ++face)
        {
            if (!first)
            {
                ASSERT_TRUE(graph.Begin());
            }
            first                = false;
            lighting.environment = Vec4(1, rotation, 1, 0);
            ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 0, true));
            const FilterReadback value = CaptureFilterCell(graph, *renderer, capture,
                                                           StaticID({10, 10, 10}), GI_STATIC, face);
            ASSERT_EQ(value.status.z, 0);
            Vec3 expected(0);
            for (uint32_t ray = 0; ray < GI_FACE_RAYS; ++ray)
            {
                const Vec3 direction(
                    FaceOracleQuery({10, 10, 10}, GI_STATIC, face, ray).directionMax);
                const Vec3 rotated(
                    std::cos(rotation) * direction.x - std::sin(rotation) * direction.z,
                    -direction.y,
                    std::sin(rotation) * direction.x + std::cos(rotation) * direction.z);
                uint32_t axis = std::abs(rotated.x) >= std::abs(rotated.y) ? 0 : 1;
                if (std::abs(rotated.z) > std::abs(rotated[axis]))
                {
                    axis = 2;
                }
                expected += Vec3(colors[axis * 2 + (rotated[axis] < 0 ? 1 : 0)]);
            }
            expected *= glm::pi<float>() / GI_FACE_RAYS;
            for (uint32_t c = 0; c < 3; ++c)
            {
                EXPECT_NEAR(value.raw[c], expected[c], 0.003f);
            }
        }
    }
}
