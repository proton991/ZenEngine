TEST_P(DynamicVoxelGIIntegrationTest, FilteringPreservesConstantFieldsAndPadding)
{
    RenderGraph& graph               = *device->GetCurrentFrameRDG();
    DynamicVoxelGIRenderer* renderer = StaticRenderer(3);
    const FilterCapture capture      = CreateFilterCapture();
    ASSERT_TRUE(graph.Begin());
    StaticFixture fixture = StaticInputs(
        graph, {{{10, 10, 10}, GI_STATIC}, {{0, 0, 0}, GI_STATIC}, {{63, 63, 63}, GI_STATIC}});
    const HeapVector<GIHit> responses      = ConstantResponses(3, fixture.ids[0]);
    const DeterministicGIProvider provider = ReferenceStatic(graph, responses, 1);
    const SceneUniformData lighting        = StaticLighting();
    HeapVector<IrradianceProbe> probes;
    for (const Vec3 position : {Vec3(0.01f), Vec3(10.01f), Vec3(10.5f), Vec3(10.99f), Vec3(63.99f)})
    {
        probes.push_back({Vec4(position, 0), Vec4(glm::normalize(Vec3(1, 2, 3)), 0)});
    }
    bool first = true;
    for (GITemporalMode temporal :
         {GITemporalMode::eOff, GITemporalMode::eFixed, GITemporalMode::eElapsed})
    {
        for (bool spatial : {false, true})
        {
            if (!first)
            {
                ASSERT_TRUE(graph.Begin());
            }
            first = false;
            renderer->SetFiltering(temporal, spatial);
            fixture.inputs.timeSeconds = 0;
            ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, true));
            const FilterReadback value =
                CaptureFilterCell(graph, *renderer, capture, fixture.ids[0]);
            ASSERT_EQ(value.status.z, 0);
            EXPECT_EQ(value.raw, value.history);
            EXPECT_EQ(value.raw, value.filtered);
            EXPECT_EQ(value.metadata.w, 1);
            ASSERT_TRUE(graph.Begin());
            const HeapVector<Vec4> sampled = ProbeStatic(graph, *renderer, probes);
            for (const Vec4& sample : sampled)
            {
                EXPECT_EQ(sample.w, 1);
                for (uint32_t c = 0; c < 3; ++c)
                {
                    EXPECT_NEAR(sample[c], value.raw[c], 0.002f);
                }
            }
        }
    }
}

TEST_P(DynamicVoxelGIIntegrationTest, TemporalStepResponseAndExplicitResets)
{
    RenderGraph& graph               = *device->GetCurrentFrameRDG();
    DynamicVoxelGIRenderer* renderer = StaticRenderer(1);
    const FilterCapture capture      = CreateFilterCapture();
    ASSERT_TRUE(graph.Begin());
    StaticFixture fixture                  = StaticInputs(graph, {{{10, 10, 10}, GI_STATIC}});
    const HeapVector<GIHit> responses      = ConstantResponses(1, fixture.ids[0]);
    const DeterministicGIProvider provider = ReferenceStatic(graph, responses, 1);
    bool initialFrame                      = true;
    for (GITemporalMode mode : {GITemporalMode::eFixed, GITemporalMode::eElapsed})
    {
        for (uint32_t hz : {30u, 60u, 120u})
        {
            renderer->SetFiltering(mode, false);
            ++fixture.inputs.historyRevision;
            SceneUniformData lighting           = StaticLighting();
            lighting.lights[0].colorIntensity.w = 0;
            fixture.inputs.timeSeconds          = 0;
            if (!initialFrame)
            {
                ASSERT_TRUE(graph.Begin());
            }
            initialFrame = false;
            ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, true));
            const FilterReadback black =
                CaptureFilterCell(graph, *renderer, capture, fixture.ids[0]);
            ASSERT_EQ(black.status.z, 0);
            EXPECT_EQ(black.history, Vec4(0, 0, 0, 1));
            EXPECT_EQ(black.metadata.w, 1); // Black radiance is valid history.
            lighting.lights[0].colorIntensity.w = glm::pi<float>();
            for (uint32_t frame = 1; frame <= hz; ++frame)
            {
                fixture.inputs.timeSeconds = double(frame) / hz;
                ASSERT_TRUE(graph.Begin());
                ASSERT_TRUE(
                    renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, true));
                if (frame == hz / 10 || frame == hz * 3 / 10 || frame == hz)
                {
                    const FilterReadback value =
                        CaptureFilterCell(graph, *renderer, capture, fixture.ids[0]);
                    ASSERT_EQ(value.status.z, 0);
                    const double exponent =
                        mode == GITemporalMode::eFixed ? frame : 60.0 * frame / hz;
                    const float expected = value.raw.z * float(1 - std::pow(0.97, exponent));
                    EXPECT_NEAR(value.history.z, expected, 0.0001f)
                        << "Hz=" << hz << " frame=" << frame;
                    EXPECT_NEAR(value.filtered.z, value.history.z, 0.002f);
                    LOGI("M5 step mode={} Hz={} frame={} raw={} history={} expected={}",
                         uint32_t(mode), hz, frame, value.raw.z, value.history.z, expected);
                }
                else
                {
                    ASSERT_TRUE(FinishStatic(graph, *renderer));
                }
            }
        }
    }
    // Long gaps and occupant/structural changes initialize directly, without stale energy.
    SceneUniformData lighting = StaticLighting();
    for (uint32_t reset = 0; reset < 5; ++reset)
    {
        ASSERT_TRUE(graph.Begin());
        fixture.inputs.timeSeconds += reset == 0 ? 0.31 : 1.0 / 60;
        if (reset == 1)
        {
            ReplaceOwner(graph, fixture.inputs.owner, {10, 10, 10}, 42);
        }
        if (reset == 2)
        {
            ++fixture.inputs.historyRevision;
        }
        if (reset == 3)
        {
            renderer->OnRenderGraphExecuted(false);
        }
        lighting.lights[0].colorIntensity.w = float(reset + 1);
        DeterministicGIProvider selected    = provider;
        if (reset == 4)
        {
            selected = ReferenceStatic(graph, responses, 2);
        }
        ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, selected, lighting, 1, true));
        const FilterReadback value = CaptureFilterCell(graph, *renderer, capture, fixture.ids[0]);
        ASSERT_EQ(value.status.z, 0);
        EXPECT_EQ(value.raw, value.history) << reset;
        EXPECT_EQ(value.metadata.w, 1);
    }
}

TEST_P(DynamicVoxelGIIntegrationTest, QualityConvergenceStationaryVarianceAndLightRemoval)
{
    RenderGraph& graph               = *device->GetCurrentFrameRDG();
    DynamicVoxelGIRenderer* renderer = StaticRenderer(1);
    const FilterCapture capture      = CreateFilterCapture();
    ASSERT_TRUE(graph.Begin());
    StaticFixture fixture                  = StaticInputs(graph, {{{10, 10, 10}, GI_STATIC}});
    const HeapVector<GIHit> responses      = ConstantResponses(1, fixture.ids[0]);
    const DeterministicGIProvider provider = ReferenceStatic(graph, responses, 1);
    bool first                             = true;
    for (GITemporalMode mode : {GITemporalMode::eFixed, GITemporalMode::eElapsed})
    {
        renderer->SetFiltering(mode, true);
        ++fixture.inputs.historyRevision;
        fixture.inputs.timeSeconds          = 0;
        SceneUniformData lighting           = StaticLighting();
        lighting.lights[0].colorIntensity.w = 0;
        if (!first)
        {
            ASSERT_TRUE(graph.Begin());
        }
        first = false;
        ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, true));
        const FilterReadback black = CaptureFilterCell(graph, *renderer, capture, fixture.ids[0]);
        ASSERT_EQ(black.status.z, 0);
        ASSERT_EQ(black.history, Vec4(0, 0, 0, 1));
        lighting.lights[0].colorIntensity.w = glm::pi<float>();
        float fullScale                     = 0;
        double sum = 0, sumSquared = 0;
        float settled = 0;
        // 320 frames at alpha=.03 leave 0.00585% of the initial error. Observe
        // another 32 frames, rather than declaring three frames converged.
        for (uint32_t frame = 1; frame <= 352; ++frame)
        {
            fixture.inputs.timeSeconds = double(frame) / 60;
            ASSERT_TRUE(graph.Begin());
            ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, true));
            if (frame == 1 || frame == 32 || frame == 128 || frame >= 320)
            {
                const FilterReadback value =
                    CaptureFilterCell(graph, *renderer, capture, fixture.ids[0]);
                ASSERT_EQ(value.status.z, 0);
                ASSERT_GT(value.raw.z, 0);
                fullScale             = value.raw.z;
                const double expected = fullScale * (1 - std::pow(0.97, frame));
                EXPECT_NEAR(value.history.z, expected, 0.003 * fullScale);
                if (frame > 320)
                {
                    const double normalized = value.history.z / fullScale;
                    sum += normalized;
                    sumSquared += normalized * normalized;
                    EXPECT_NEAR(normalized, 1.0, 0.001);
                }
                settled = value.history.z;
                if (frame == 1 || frame == 32 || frame == 128 || frame == 320)
                {
                    LOGI("M7 quality startup mode={} frame={} normalized={} expected={}",
                         uint32_t(mode), frame, value.history.z / fullScale, expected / fullScale);
                }
            }
            else
            {
                ASSERT_TRUE(FinishStatic(graph, *renderer));
            }
        }
        const double variance = std::max(0.0, sumSquared / 32 - (sum / 32) * (sum / 32));
        EXPECT_LE(std::sqrt(variance), 0.001);
        LOGI("M7 quality stationary mode={} samples=32 relative_rms={}", uint32_t(mode),
             std::sqrt(variance));
        lighting.lights[0].colorIntensity.w = 0;
        for (uint32_t frame = 1; frame <= 320; ++frame)
        {
            fixture.inputs.timeSeconds = double(352 + frame) / 60;
            ASSERT_TRUE(graph.Begin());
            ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, true));
            if (frame == 1 || frame == 10 || frame == 32 || frame == 64 || frame == 128 ||
                frame == 320)
            {
                const FilterReadback value =
                    CaptureFilterCell(graph, *renderer, capture, fixture.ids[0]);
                ASSERT_EQ(value.status.z, 0);
                EXPECT_EQ(value.raw, Vec4(0, 0, 0, 1));
                const double expected = settled * std::pow(0.97, frame);
                EXPECT_NEAR(value.history.z, expected, 0.003 * fullScale);
                EXPECT_NEAR(value.filtered.z, value.history.z, 0.003 * fullScale);
                if (frame == 320)
                {
                    EXPECT_LE(value.history.z / fullScale, 0.001);
                }
                LOGI("M7 quality removal mode={} frame={} normalized={} expected={}",
                     uint32_t(mode), frame, value.history.z / fullScale, expected / fullScale);
            }
            else
            {
                ASSERT_TRUE(FinishStatic(graph, *renderer));
            }
        }
    }
}

TEST_P(DynamicVoxelGIIntegrationTest, GaussianUsesValidNeighborsWithoutFillingEmptyCells)
{
    RenderGraph& graph               = *device->GetCurrentFrameRDG();
    DynamicVoxelGIRenderer* renderer = StaticRenderer(3);
    renderer->SetFiltering(GITemporalMode::eOff, true);
    const FilterCapture capture = CreateFilterCapture();
    ASSERT_TRUE(graph.Begin());
    StaticFixture fixture = StaticInputs(
        graph, {{{10, 10, 10}, GI_STATIC}, {{11, 10, 10}, GI_STATIC}, {{11, 11, 10}, GI_STATIC}});
    HeapVector<GIHit> responses = ConstantResponses(3, fixture.ids[0]);
    for (uint32_t face = 0; face < 6; ++face)
    {
        for (uint32_t receiver = 0; receiver < 3; ++receiver)
        {
            for (uint32_t ray = 0; ray < GI_FACE_RAYS; ++ray)
            {
                responses[(face * 3 + receiver) * GI_FACE_RAYS + ray].diffuseReflectance =
                    Vec4(float(receiver), 0, 0, 0);
            }
        }
    }
    const DeterministicGIProvider provider = ReferenceStatic(graph, responses, 1);
    const SceneUniformData lighting        = StaticLighting();
    ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, false));
    const FilterReadback value = CaptureFilterCell(graph, *renderer, capture, fixture.ids[0]);
    ASSERT_EQ(value.status.z, 0);
    EXPECT_EQ(value.raw, Vec4(0, 0, 0, 1));
    // Center/axis/diagonal Gaussian weights are 8/4/2; independently normalized.
    EXPECT_NEAR(value.filtered.x, glm::pi<float>() * (4 + 2 * 2) / 14, 0.003f);
    ASSERT_TRUE(graph.Begin());
    const FilterReadback padding =
        CaptureFilterCell(graph, *renderer, capture, StaticID({9, 10, 10}));
    EXPECT_EQ(padding.history, Vec4(0));
    EXPECT_EQ(padding.filtered, value.filtered);
    // The two bright donors are now the opposite side of a thin wall. They
    // must not brighten the dark receiver through the spatial filter.
    ASSERT_TRUE(graph.Begin());
    const uint32_t oppositeNormal = 0x00008080;
    RHIBuffer* upload             = Buffer(4, RHIBufferAllocateType::eCPUWrite, &oppositeNormal);
    graph.GetResourceManager()->ImportHostWrittenBuffer(upload);
    for (const Vec3i cell : {Vec3i(11, 10, 10), Vec3i(11, 11, 10)})
    {
        RHIBufferTextureCopyRegion region{};
        region.textureOffset = cell;
        region.textureSize   = Vec3i(1);
        region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
        graph.AddTransferPass("OppositeFilterSurface")
            .CopyBufferToTexture(upload, fixture.inputs.normal, region);
    }
    ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, false));
    const FilterReadback separated = CaptureFilterCell(graph, *renderer, capture, fixture.ids[0]);
    EXPECT_EQ(separated.status.z, 0);
    EXPECT_EQ(separated.filtered, Vec4(0, 0, 0, 1));
}

TEST_P(DynamicVoxelGIIntegrationTest, FilteredProviderSubstitutionIncludesComposition)
{
    CheckProviderSubstitution(GITemporalMode::eElapsed, true);
    CheckProviderSubstitution(GITemporalMode::eFixed, false);
    CheckProviderSubstitution(GITemporalMode::eOff, true);
}

TEST_P(DynamicVoxelGIIntegrationTest, DynamicCooldownBoundsMotionTrailsForBothNeighborhoods)
{
    RenderGraph& graph          = *device->GetCurrentFrameRDG();
    const FilterCapture capture = CreateFilterCapture();
    for (uint32_t radius : {1u, 2u})
    {
        for (uint32_t filtering = 0; filtering < 3; ++filtering)
        {
            DynamicVoxelGIRenderer* renderer = StaticRenderer(1, radius);
            renderer->SetFiltering(filtering == 0 ? GITemporalMode::eOff : GITemporalMode::eElapsed,
                                   filtering == 2);
            VoxelDDAProvider provider;
            ASSERT_TRUE(graph.Begin());
            StaticFixture fixture =
                FrameInputs(graph, {{{10, 10, 12}, GI_STATIC}, {{10, 10, 10}, GI_DYNAMIC}}, 1);
            fixture.inputs.timeSeconds = 0;
            ASSERT_TRUE(provider.Prepare(fixture.staticVoxels, fixture.dynamicVoxels,
                                         fixture.inputs.grid, 1));
            const SceneUniformData lighting = StaticLighting();
            ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, false));
            const uint32_t receiver = StaticID({9, 10, 10});
            const FilterReadback warm =
                CaptureFilterCell(graph, *renderer, capture, receiver, GI_DYNAMIC, 4);
            ASSERT_EQ(warm.status.z, 0);
            ASSERT_GT(warm.raw.z, 0);
            EXPECT_EQ(warm.raw, warm.history);
            ASSERT_TRUE(graph.Begin());
            fixture =
                FrameInputs(graph, {{{10, 10, 12}, GI_STATIC}, {{14, 10, 10}, GI_DYNAMIC}}, 1);
            fixture.inputs.timeSeconds = 0.1;
            ASSERT_TRUE(provider.Prepare(fixture.staticVoxels, fixture.dynamicVoxels,
                                         fixture.inputs.grid, 2));
            ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, false));
            const FilterReadback moving =
                CaptureFilterCell(graph, *renderer, capture, receiver, GI_DYNAMIC, 4);
            ASSERT_EQ(moving.status.z, 0);
            EXPECT_EQ(moving.metadata.w, filtering == 0 ? 0 : 1);
            LOGI("M5 motion radius={} filters={} raw={} temporal={} final={}", radius, filtering,
                 moving.raw.z, moving.history.z, moving.filtered.z);
            for (double time : {0.299, 0.301})
            {
                ASSERT_TRUE(graph.Begin());
                fixture.inputs.timeSeconds = time;
                ASSERT_TRUE(
                    renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, false));
                const FilterReadback value =
                    CaptureFilterCell(graph, *renderer, capture, receiver, GI_DYNAMIC, 4);
                ASSERT_EQ(value.status.z, 0);
                EXPECT_EQ(value.metadata.w, filtering != 0 && time < 0.3 ? 1 : 0);
                if (time > 0.3)
                {
                    EXPECT_EQ(value.filtered, Vec4(0));
                }
            }
            // Removing the object invalidates its current occupied neighborhood immediately.
            ASSERT_TRUE(graph.Begin());
            fixture                        = FrameInputs(graph, {{{10, 10, 12}, GI_STATIC}}, 1);
            fixture.inputs.timeSeconds     = 0.32;
            fixture.inputs.historyRevision = 1;
            ASSERT_TRUE(provider.Prepare(fixture.staticVoxels, fixture.dynamicVoxels,
                                         fixture.inputs.grid, 3));
            ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, false));
            const FilterReadback removed =
                CaptureFilterCell(graph, *renderer, capture, StaticID({14, 10, 10}), GI_DYNAMIC, 4);
            EXPECT_EQ(removed.metadata.w, 0);
            EXPECT_EQ(removed.filtered, Vec4(0));
        }
    }
}

TEST_P(DynamicVoxelGIIntegrationTest, RevealedStaticCellsInitializeBeforeFilteredComposition)
{
    RenderGraph& graph               = *device->GetCurrentFrameRDG();
    DynamicVoxelGIRenderer* renderer = StaticRenderer(2);
    renderer->SetFiltering(GITemporalMode::eElapsed, true);
    const FilterCapture capture = CreateFilterCapture();
    ASSERT_TRUE(graph.Begin());
    StaticFixture fixture =
        StaticInputs(graph, {{{10, 10, 10}, GI_STATIC}, {{40, 40, 40}, GI_STATIC}});
    const HeapVector<GIHit> responses      = ConstantResponses(2, fixture.ids[0]);
    const DeterministicGIProvider provider = ReferenceStatic(graph, responses, 1);
    SceneUniformData lighting              = StaticLighting();
    ReceiverSurface(graph, fixture.inputs, Vec3(10.5f), GI_STATIC);
    fixture.inputs.timeSeconds = 0;
    ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, true));
    const FilterReadback hidden = CaptureFilterCell(graph, *renderer, capture, fixture.ids[1]);
    EXPECT_EQ(hidden.metadata.w, 0);
    EXPECT_EQ(hidden.filtered, Vec4(0));
    ASSERT_TRUE(graph.Begin());
    ReceiverSurface(graph, fixture.inputs, Vec3(40.5f), GI_STATIC);
    fixture.inputs.timeSeconds = 1.0 / 60;
    lighting.lights[0].colorIntensity.w *= 0.5f;
    ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, true));
    const FilterReadback revealed = CaptureFilterCell(graph, *renderer, capture, fixture.ids[1]);
    EXPECT_EQ(revealed.metadata.w, 1);
    EXPECT_EQ(revealed.raw, revealed.history);
    EXPECT_EQ(revealed.history, revealed.filtered);
    EXPECT_EQ(renderer->GetCacheBuildBatches(), 1);
}
