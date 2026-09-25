TEST_P(DynamicVoxelGIIntegrationTest, FourTransportCombinationsMatchIndependentBoxIntegration)
{
    RenderGraph& graph               = *device->GetCurrentFrameRDG();
    DynamicVoxelGIRenderer* renderer = StaticRenderer(2);
    uint64_t generation              = 1;
    for (uint32_t receiverClass : {GI_STATIC, GI_DYNAMIC})
    {
        for (uint32_t senderClass : {GI_STATIC, GI_DYNAMIC})
        {
            ASSERT_TRUE(graph.Begin());
            const HeapVector<OccupiedCell> cells = {{{10, 10, 10}, receiverClass},
                                                    {{10, 10, 12}, senderClass}};
            StaticFixture fixture                = FrameInputs(graph, cells, generation);
            ReceiverSurface(graph, fixture.inputs, Vec3(10.5f, 10.5f, 10.1f), receiverClass);
            VoxelDDAProvider provider;
            ASSERT_TRUE(provider.Prepare(fixture.staticVoxels, fixture.dynamicVoxels,
                                         fixture.inputs.grid, generation++));
            ASSERT_TRUE(
                renderer->BuildRenderGraph(fixture.inputs, provider, StaticLighting(), 1, false));
            const StaticReadback output =
                CaptureStatic(graph, *renderer, receiverClass == GI_DYNAMIC);
            ASSERT_EQ(output.status.z, 0);
            const Vec3 expected = OracleGather(cells, {10, 10, 10}, receiverClass, 4);
            EXPECT_GT(expected.z, 0);
            const Vec4 actual = output.faces[2 * (4 * StaticCells + StaticID({10, 10, 10}))];
            EXPECT_EQ(actual.w, 1);
            for (uint32_t c = 0; c < 3; ++c)
            {
                EXPECT_NEAR(actual[c], expected[c], 0.0005f);
            }
        }
    }
}

TEST_P(DynamicVoxelGIIntegrationTest, CameraSelectionIncludesPaddingDonorsAndKeepsOffscreenSenders)
{
    RenderGraph& graph = *device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph.Begin());
    const HeapVector<OccupiedCell> cells = {
        {{10, 10, 10}, GI_STATIC}, {{10, 10, 14}, GI_STATIC}, {{40, 40, 40}, GI_STATIC}};
    StaticFixture fixture            = FrameInputs(graph, cells, 1);
    DynamicVoxelGIRenderer* renderer = StaticRenderer(3);
    VoxelDDAProvider provider;
    ASSERT_TRUE(
        provider.Prepare(fixture.staticVoxels, fixture.dynamicVoxels, fixture.inputs.grid, 1));
    ReceiverSurface(graph, fixture.inputs, Vec3(10.5f, 10.5f, 10.1f), GI_STATIC);
    ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, StaticLighting(), 1, false));
    const StaticReadback initial = CaptureStatic(graph, *renderer);
    EXPECT_EQ(initial.status.z, 0);
    EXPECT_EQ(initial.masks[StaticID({10, 10, 14})], 1);
    EXPECT_GT(initial.faces[2 * (4 * StaticCells + StaticID({10, 10, 10}))].z, 0);
    EXPECT_EQ(initial.faces[2 * (4 * StaticCells + StaticID({10, 10, 14}))].w, 0);
    for (uint32_t state = 0; state < 4; ++state)
    {
        ASSERT_TRUE(graph.Begin());
        ReceiverSurface(graph, fixture.inputs, state == 1 ? Vec3(40.5f) : Vec3(10.5f, 10.5f, 10.1f),
                        GI_STATIC, state == 2);
        ASSERT_TRUE(
            renderer->BuildRenderGraph(fixture.inputs, provider, StaticLighting(), 1, false));
        const FrameReadback work = CaptureWork(graph, *renderer);
        ASSERT_EQ(work.status.z, 0);
        EXPECT_EQ(work.counts.x, state == 2 ? 0 : 1);
        EXPECT_EQ(work.counts.y, 0);
        EXPECT_EQ(work.counts.z, 0);
        EXPECT_EQ(renderer->GetCacheBuildBatches(), 1);
        EXPECT_EQ(work.arguments[0].x == 0, state == 2);
        if (state != 2)
        {
            EXPECT_EQ(work.staticList[0],
                      state == 1 ? StaticID({40, 40, 40}) : StaticID({10, 10, 10}));
        }
    }
    ASSERT_TRUE(graph.Begin());
    const HeapVector<IrradianceProbe> probes = {{Vec4(10.5f, 10.5f, 10.1f, 0), Vec4(0, 0, 1, 0)}};
    const HeapVector<Vec4> values            = ProbeStatic(graph, *renderer, probes);
    EXPECT_EQ(values[0].w, 1); // z=9/10 interpolation requires the padded empty layer.
}

TEST_P(DynamicVoxelGIIntegrationTest, DynamicNeighborhoodTeleportRemovalAndStaticCacheReuse)
{
    RenderGraph& graph = *device->GetCurrentFrameRDG();
    for (uint32_t radius : {1u, 2u})
    {
        DynamicVoxelGIRenderer* renderer = StaticRenderer(1, radius);
        for (uint32_t state = 0; state < 4; ++state)
        {
            ASSERT_TRUE(graph.Begin());
            HeapVector<OccupiedCell> cells = {{{10, 10, 10}, GI_STATIC}};
            if (state < 2)
            {
                cells.push_back({state == 0 ? glm::uvec3(10, 10, 12) : glm::uvec3(40), GI_DYNAMIC});
            }
            StaticFixture fixture = FrameInputs(graph, cells, 1);
            VoxelDDAProvider provider;
            ASSERT_TRUE(provider.Prepare(fixture.staticVoxels, fixture.dynamicVoxels,
                                         fixture.inputs.grid, state + 1));
            ASSERT_TRUE(
                renderer->BuildRenderGraph(fixture.inputs, provider, StaticLighting(), 1, false));
            const FrameReadback work = CaptureWork(graph, *renderer);
            ASSERT_EQ(work.status.z, 0);
            EXPECT_EQ(work.counts.x, 1);
            EXPECT_EQ(work.counts.y,
                      state < 2 ? (2 * radius + 1) * (2 * radius + 1) * (2 * radius + 1) : 0);
            EXPECT_EQ(renderer->GetCacheBuildBatches(), 1);
            HeapVector<bool> seen(StaticCells, false);
            for (uint32_t i = 0; i < work.counts.y; ++i)
            {
                const uint32_t id = work.dynamicList[i];
                ASSERT_LT(id, StaticCells);
                EXPECT_FALSE(seen[id]);
                seen[id] = true;
                const glm::ivec3 p(id / (StaticSide * StaticSide), (id / StaticSide) % StaticSide,
                                   id % StaticSide);
                const glm::ivec3 center = state == 0 ? glm::ivec3(10, 10, 12) : glm::ivec3(40);
                EXPECT_TRUE(glm::all(glm::lessThanEqual(glm::abs(p - center), glm::ivec3(radius))));
            }
            ASSERT_TRUE(graph.Begin());
            const StaticReadback dynamic = CaptureStatic(graph, *renderer, true);
            EXPECT_EQ(dynamic.faces[2 * StaticID({10, 10, 12})].w, state == 0 ? 1 : 0);
            EXPECT_EQ(dynamic.faces[2 * StaticID({40, 40, 40})].w, state == 1 ? 1 : 0);
        }
    }
}

TEST_P(DynamicVoxelGIIntegrationTest, CacheInitializationIsBatchedAndFailedPublicationRestarts)
{
    RenderGraph& graph = *device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph.Begin());
    HeapVector<OccupiedCell> cells;
    for (uint32_t i = 0; i < GI_CACHE_BATCH + 1; ++i)
    {
        cells.push_back({glm::uvec3(8 + i / 256, 8 + (i / 16) % 16, 8 + i % 16), GI_STATIC});
    }
    StaticFixture fixture            = FrameInputs(graph, cells, 1);
    DynamicVoxelGIRenderer* renderer = StaticRenderer(GI_CACHE_BATCH + 1);
    VoxelDDAProvider provider;
    ASSERT_TRUE(
        provider.Prepare(fixture.staticVoxels, fixture.dynamicVoxels, fixture.inputs.grid, 1));
    for (uint32_t frame = 0; frame < 4; ++frame)
    {
        if (frame != 0)
        {
            ASSERT_TRUE(graph.Begin());
        }
        if (frame == 3)
        {
            renderer->OnRenderGraphExecuted(false);
        }
        ReceiverSurface(graph, fixture.inputs, Vec3(0), GI_STATIC, true);
        ASSERT_TRUE(
            renderer->BuildRenderGraph(fixture.inputs, provider, StaticLighting(), 1, false));
        const FrameReadback work = CaptureWork(graph, *renderer);
        EXPECT_EQ(work.status.z, frame == 0 || frame == 3 ? GI_CACHE_PENDING : 0);
        EXPECT_EQ(work.counts.x, 0);
        EXPECT_EQ(work.counts.z, frame == 0 || frame == 3 ? GI_CACHE_BATCH : frame == 1 ? 1 : 0);
        EXPECT_EQ(renderer->GetCacheBuildBatches(), frame == 0 ? 1 : frame == 3 ? 3 : 2);
    }
}

TEST_P(DynamicVoxelGIIntegrationTest, GPUArgumentsRespectMockedLimitsAndCoverEveryReceiverOnce)
{
    for (uint32_t count : {0u, 1u, 63u, 64u, 65u, 127u, 128u, 129u, 255u, 256u, 257u, 513u, 1025u})
    {
        CheckIndirectCoverage(count);
    }
}

TEST_P(DynamicVoxelGIIntegrationTest, MovingUnlitBlockerKeepsCachedStaticFirstHitOpaque)
{
    RenderGraph& graph               = *device->GetCurrentFrameRDG();
    DynamicVoxelGIRenderer* renderer = StaticRenderer(5);
    float blocked = 0, visible = 0;
    HeapVector<GIHit> cache;
    SceneUniformData light           = StaticLighting();
    light.lights[0].directionType.w  = 1;
    light.lights[0].positionRange    = Vec4(20.5f, 10.5f, 15.5f, 1000);
    light.lights[0].colorIntensity.w = 100;
    for (uint32_t state = 0; state < 3; ++state)
    {
        ASSERT_TRUE(graph.Begin());
        HeapVector<OccupiedCell> cells = {{{10, 10, 14}, GI_STATIC},
                                          {{10, 10, 10}, GI_STATIC},
                                          {{13, 9, 13}, GI_STATIC},
                                          {{13, 10, 13}, GI_STATIC},
                                          {{13, 11, 13}, GI_STATIC}};
        if (state < 2)
        {
            cells.push_back({state == 0 ? glm::uvec3(10, 10, 12) : glm::uvec3(40), GI_DYNAMIC});
        }
        StaticFixture fixture = FrameInputs(graph, cells, 1);
        ReceiverSurface(graph, fixture.inputs, Vec3(10.5f, 10.5f, 14.1f), GI_STATIC);
        VoxelDDAProvider provider;
        ASSERT_TRUE(provider.Prepare(fixture.staticVoxels, fixture.dynamicVoxels,
                                     fixture.inputs.grid, state + 1));
        ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, light, 1, true));
        const StaticReadback output = CaptureStatic(graph, *renderer);
        ASSERT_EQ(output.status.z, 0);
        EXPECT_EQ(output.masks[StaticID({10, 10, 10})], 1);
        EXPECT_EQ(renderer->GetCacheBuildBatches(), 1);
        const float irradiance = output.faces[2 * (5 * StaticCells + StaticID({10, 10, 14}))].z;
        if (state == 0)
        {
            cache   = output.hits;
            blocked = irradiance;
            ASSERT_TRUE(graph.Begin());
            const StaticReadback moving = CaptureStatic(graph, *renderer, true);
            EXPECT_EQ(moving.masks[StaticID({10, 10, 12})], 0);
        }
        else
        {
            EXPECT_EQ(std::memcmp(cache.data(), output.hits.data(), cache.size() * sizeof(GIHit)),
                      0);
            EXPECT_GT(irradiance, blocked + 1e-5f);
            if (state == 1)
            {
                visible = irradiance;
            }
            else
            {
                EXPECT_EQ(irradiance, visible);
            }
        }
    }
}
