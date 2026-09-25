TEST_P(DynamicVoxelGIIntegrationTest, SampleCountsPreserveConstantFieldEnergy)
{
    RenderGraph& graph = *device->GetCurrentFrameRDG();
    for (uint32_t rays : {32u, 64u, 128u})
    {
        ASSERT_TRUE(graph.Begin());
        const StaticFixture fixture      = StaticInputs(graph, {{{10, 10, 10}, GI_STATIC}});
        DynamicVoxelGIRenderer* renderer = StaticRenderer(1, 2, StaticSide, false, rays);
        renderer->SetLighting(false, false, true);
        GIHit sender{};
        sender.identity = glm::uvec4(GI_HIT, GI_STATIC, fixture.ids[0], GI_SURFACE_VALID);
        sender.emission = Vec4(2, 3, 4, 0);
        const HeapVector<GIHit> responses(GI_FACE_COUNT * rays, sender);
        const DeterministicGIProvider provider = ReferenceStatic(graph, responses, 1);
        const SceneUniformData lighting{};
        ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, false));
        const StaticReadback result = CaptureStatic(graph, *renderer);
        ASSERT_EQ(result.status.z, 0);
        for (uint32_t face = 0; face < GI_FACE_COUNT; ++face)
        {
            const Vec4 actual = result.faces[2 * (face * StaticCells + fixture.ids[0])];
            EXPECT_EQ(actual.w, 1);
            for (uint32_t channel = 0; channel < 3; ++channel)
            {
                EXPECT_NEAR(actual[channel], glm::pi<float>() * sender.emission[channel], 0.005f);
            }
        }
    }
}

TEST_P(DynamicVoxelGIIntegrationTest, CompactCachePreservesIdentityDistanceAndLiveMaterials)
{
    RenderGraph& graph = *device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph.Begin());
    const HeapVector<OccupiedCell> cells = {{{0, 0, 0}, GI_STATIC},     {{63, 63, 63}, GI_STATIC},
                                            {{10, 10, 10}, GI_STATIC},  {{10, 10, 11}, GI_STATIC},
                                            {{11, 10, 10}, GI_STATIC},  {{10, 11, 10}, GI_STATIC},
                                            {{10, 10, 11}, GI_DYNAMIC}, {{10, 10, 9}, GI_DYNAMIC}};
    StaticFixture fixture                = FrameInputs(graph, cells, 1);
    VoxelDDAProvider provider;
    ASSERT_TRUE(
        provider.Prepare(fixture.staticVoxels, fixture.dynamicVoxels, fixture.inputs.grid, 1));
    const uint32_t capacity         = static_cast<uint32_t>(fixture.ids.size()) + 1;
    DynamicVoxelGIRenderer* decoded = StaticRenderer(capacity);
    DynamicVoxelGIRenderer* compact = StaticRenderer(capacity, 2, StaticSide, true);
    SceneUniformData lighting       = StaticLighting();
    for (uint32_t state = 0; state < 2; ++state)
    {
        if (state != 0)
        {
            ASSERT_TRUE(graph.Begin());
            graph.AddTransferPass("ChangeCompactCacheMaterial")
                .ClearTexture(fixture.staticVoxels.pAlbedo, Color(0.2f, 0.4f, 0.6f, 1.0f))
                .ClearTexture(fixture.staticVoxels.pReflectance, Color(0.1f, 0.2f, 0.3f, 1.0f));
        }
        ASSERT_TRUE(decoded->BuildRenderGraph(fixture.inputs, provider, lighting, 1, true));
        const StaticReadback expected = CaptureStatic(graph, *decoded);
        ASSERT_TRUE(graph.Begin());
        ASSERT_TRUE(compact->BuildRenderGraph(fixture.inputs, provider, lighting, 1, true));
        const StaticReadback actual = CaptureStatic(graph, *compact);
        EXPECT_EQ(actual.status, expected.status);
        ASSERT_EQ(actual.faces.size(), expected.faces.size());
        EXPECT_EQ(std::memcmp(actual.faces.data(), expected.faces.data(),
                              actual.faces.size() * sizeof(Vec4)),
                  0);
        ASSERT_EQ(actual.compactHits.size(), expected.hits.size());
        for (uint32_t index = 0; index < expected.hits.size(); ++index)
        {
            const GIHit& hit        = expected.hits[index];
            const glm::uvec2 packed = actual.compactHits[index];
            EXPECT_EQ((packed.x >> 26) & 3u, hit.identity.x);
            if (hit.identity.x == GI_HIT)
            {
                EXPECT_EQ((packed.x >> 24) & 3u, hit.identity.y);
                EXPECT_EQ(packed.x & 0xffffffu, hit.identity.z);
            }
            uint32_t distanceBits = 0;
            std::memcpy(&distanceBits, &hit.positionDistance.w, sizeof(distanceBits));
            EXPECT_EQ(packed.y, distanceBits);
        }
        EXPECT_EQ(compact->GetCacheBuildBatches(), 1);
    }
}

TEST_P(DynamicVoxelGIIntegrationTest, DirectionalCompositionPreservesAnalyticFirstMoment)
{
    RenderGraph& graph = *device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph.Begin());
    const StaticFixture fixture      = StaticInputs(graph, {{{10, 10, 10}, GI_STATIC}});
    DynamicVoxelGIRenderer* renderer = StaticRenderer(1);
    renderer->SetLighting(false, false, true);
    // For incident radiance L(w)=a+b.w, the cosine-hemisphere integral is
    // pi*a+(2*pi/3)*b.n. This positive field has an analytic answer at any normal.
    constexpr float constant = 2.0f;
    const Vec3 gradient(0.4f, -0.6f, 0.8f);
    HeapVector<GIHit> responses(GI_FACE_COUNT * GI_FACE_RAYS);
    for (uint32_t face = 0; face < GI_FACE_COUNT; ++face)
    {
        const float sign = face % 2 == 0 ? 1.0f : -1.0f;
        GIHit sender{};
        sender.identity = glm::uvec4(GI_HIT, GI_STATIC, fixture.ids[0], GI_SURFACE_VALID);
        sender.emission = Vec4(Vec3(constant + 2.0f / 3.0f * sign * gradient[face / 2]), 0);
        for (uint32_t ray = 0; ray < GI_FACE_RAYS; ++ray)
        {
            responses[face * GI_FACE_RAYS + ray] = sender;
        }
    }
    const DeterministicGIProvider provider = ReferenceStatic(graph, responses, 1);
    SceneUniformData lighting{};
    ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, true));
    ASSERT_TRUE(FinishStatic(graph, *renderer));
    HeapVector<IrradianceProbe> probes;
    for (const Vec3 normal : {Vec3(1, 0, 0), Vec3(-1, 0, 0), Vec3(1, 1, 0), Vec3(1, 1, 1),
                              Vec3(-1, 2, -3), Vec3(2, -3, 1)})
    {
        probes.push_back({Vec4(10.5f, 10.5f, 10.5f, 0), Vec4(glm::normalize(normal), 0)});
    }
    ASSERT_TRUE(graph.Begin());
    const HeapVector<Vec4> sampled = ProbeStatic(graph, *renderer, probes);
    for (uint32_t index = 0; index < sampled.size(); ++index)
    {
        const float expected = glm::pi<float>() *
            (constant + 2.0f / 3.0f * glm::dot(gradient, Vec3(probes[index].normal)));
        EXPECT_EQ(sampled[index].w, 1);
        for (uint32_t channel = 0; channel < 3; ++channel)
        {
            EXPECT_NEAR(sampled[index][channel], expected, 0.005f);
        }
    }
    // Negative reconstruction weights cannot hide an uninitialized opposite face.
    ASSERT_TRUE(graph.Begin());
    graph.AddTransferPass("InvalidateOppositeFace")
        .ClearTexture(renderer->GetPaddedIrradiance(1), Color(0));
    const HeapVector<Vec4> invalid = ProbeStatic(graph, *renderer, probes);
    EXPECT_EQ(invalid[0].w, 1);
    EXPECT_EQ(invalid[1].w, 0);
    EXPECT_EQ(invalid[2].w, 0);
}

TEST_P(DynamicVoxelGIIntegrationTest, StaticConstantFieldThresholdPaddingAndOverflow)
{
    RenderGraph& graph = *device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph.Begin());
    const HeapVector<OccupiedCell> cells = {
        {{10, 10, 10}, GI_STATIC}, {{0, 0, 0}, GI_STATIC}, {{63, 63, 63}, GI_STATIC}};
    StaticFixture fixture            = StaticInputs(graph, cells);
    DynamicVoxelGIRenderer* renderer = StaticRenderer(3);
    SceneUniformData lighting        = StaticLighting();
    GIHit sender{};
    sender.identity =
        glm::uvec4(GI_HIT, GI_STATIC, fixture.ids[0], GI_CELL_PRECISION | GI_SURFACE_VALID);
    sender.normal             = Vec4(0, 0, 1, 0);
    sender.diffuseReflectance = Vec4(0.25f, 0.5f, 1, 0);
    GIHit miss{};
    miss.identity = glm::uvec4(GI_MISS, 0, GI_INVALID_CELL, 0);
    HeapVector<GIHit> responses(3 * GI_FACE_RAYS * GI_FACE_COUNT + 24, sender);
    const uint32_t lightOffset = 3 * GI_FACE_RAYS * GI_FACE_COUNT;
    for (uint32_t i = lightOffset; i < responses.size(); ++i)
    {
        responses[i] = miss;
    }
    DeterministicGIProvider reference = ReferenceStatic(graph, responses, 1);
    ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, reference, lighting, 1, true));
    const StaticReadback constant = CaptureStatic(graph, *renderer);
    ASSERT_EQ(constant.status, glm::uvec4(3, 3, 0, 0));
    const Vec3 expected = Vec3(sender.diffuseReflectance) * glm::pi<float>();
    for (uint32_t face = 0; face < GI_FACE_COUNT; ++face)
    {
        for (uint32_t id : fixture.ids)
        {
            const Vec4 raw = constant.faces[2 * (face * StaticCells + id)];
            for (uint32_t c = 0; c < 3; ++c)
            {
                EXPECT_NEAR(raw[c], expected[c], 0.002f);
            }
            EXPECT_EQ(raw.w, 1);
            EXPECT_EQ(constant.masks[id], 1);
        }
        EXPECT_EQ(constant.faces[2 * (face * StaticCells + StaticID({10, 10, 9}))], Vec4(0));
        EXPECT_EQ(constant.faces[2 * (face * StaticCells + StaticID({10, 10, 9})) + 1].w, 1);
        EXPECT_EQ(constant.faces[2 * (face * StaticCells + StaticID({10, 10, 8})) + 1], Vec4(0));
    }
    // Fractional plane positions, boundaries, grid edges and axis/diagonal shading normals.
    HeapVector<IrradianceProbe> probes;
    for (const OccupiedCell& cell : cells)
    {
        for (float t : {0.0f, 0.1f, 0.25f, 0.5f, 0.9f, 0.999f, 1.0f})
        {
            for (Vec3 normal :
                 {Vec3(1, 0, 0), Vec3(0, -1, 0), Vec3(0, 0, 1), glm::normalize(Vec3(-1, 2, -3))})
            {
                probes.push_back({Vec4(Vec3(cell.cell) + Vec3(t), 0), Vec4(normal, 0)});
            }
        }
    }
    ASSERT_TRUE(graph.Begin());
    const HeapVector<Vec4> values = ProbeStatic(graph, *renderer, probes);
    for (const Vec4& value : values)
    {
        EXPECT_EQ(value.w, 1);
        for (uint32_t c = 0; c < 3; ++c)
        {
            EXPECT_NEAR(value[c], expected[c], 0.002f);
        }
    }
    ASSERT_TRUE(graph.Begin());
    const HeapVector<Vec4> composed =
        ComposeStatic(graph, *renderer, lighting, Vec3(10.1f), glm::normalize(Vec3(1, 2, 3)));
    const Vec3 outgoing = Vec3(sender.diffuseReflectance) * Vec3(0.5f, 0.25f, 1) * 0.96f;
    for (uint32_t pixel = 0; pixel < composed.size() / ZEN_LIGHTING_CAPTURE_COMPONENTS; ++pixel)
    {
        const Vec4 diffuse = composed[pixel * ZEN_LIGHTING_CAPTURE_COMPONENTS + 2];
        EXPECT_EQ(diffuse.w, 1);
        for (uint32_t c = 0; c < 3; ++c)
        {
            EXPECT_NEAR(diffuse[c], outgoing[c], 0.0005f);
        }
    }
    // Exactly four visible samples are unlit; five are lit. Unlit geometry is
    // still a cached hit, so it contributes zero rather than exposing a sender.
    for (uint32_t visible : {4u, 5u})
    {
        ASSERT_TRUE(graph.Begin());
        for (uint32_t sample = 0; sample < 8; ++sample)
        {
            responses[lightOffset + sample] = sample < visible ? miss : sender;
        }
        reference = ReferenceStatic(graph, responses, visible);
        ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, reference, lighting, 1, true));
        const StaticReadback threshold = CaptureStatic(graph, *renderer);
        ASSERT_EQ(threshold.status.z, 0);
        EXPECT_EQ(threshold.masks[fixture.ids[0]], visible > 4 ? 1 : 0);
        const Vec4 raw = threshold.faces[2 * fixture.ids[0]];
        EXPECT_NEAR(raw.z, visible > 4 ? glm::pi<float>() : 0.0f, 0.002f);
    }
    // Point/spot energy uses the same engine attenuation at a known hit point.
    sender.positionDistance = Vec4(10.5f, 10.5f, 10.5f, 1);
    for (uint32_t index = 0; index < lightOffset; ++index)
    {
        responses[index] = sender;
    }
    for (uint32_t index = lightOffset; index < responses.size(); ++index)
    {
        responses[index] = miss;
    }
    for (uint32_t type : {1u, 2u})
    {
        ASSERT_TRUE(graph.Begin());
        reference                          = ReferenceStatic(graph, responses, 10 + type);
        lighting.lights[0].positionRange   = Vec4(10.5f, 10.5f, 20, 64);
        lighting.lights[0].directionType.w = static_cast<float>(type);
        lighting.lights[0].coneShadow =
            Vec4(std::cos(glm::radians(15.0f)), std::cos(glm::radians(30.0f)), 1, 0);
        ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, reference, lighting, 1, true));
        const StaticReadback point = CaptureStatic(graph, *renderer);
        ASSERT_EQ(point.status.z, 0);
        const float cutoff   = 1.0f - std::pow(9.5f / 64.0f, 4.0f);
        const float incident = glm::pi<float>() * cutoff * cutoff / (9.5f * 9.5f + 0.01f);
        EXPECT_NEAR(point.faces[2 * fixture.ids[0]].z, incident, 3e-5f);
    }
    ASSERT_TRUE(graph.Begin());
    DynamicVoxelGIRenderer* limitedRenderer = StaticRenderer(2);
    ASSERT_TRUE(limitedRenderer->BuildRenderGraph(fixture.inputs, reference, lighting, 1, true));
    const StaticReadback overflow = CaptureStatic(graph, *limitedRenderer);
    EXPECT_EQ(overflow.status, glm::uvec4(3, 2, GI_STATIC_OVERFLOW, 0));
    EXPECT_EQ(overflow.faces[2 * fixture.ids[0]], Vec4(0));
}

TEST_P(DynamicVoxelGIIntegrationTest, TranslatedPlanarReceiversKeepConstantIrradiance)
{
    RenderGraph& graph               = *device->GetCurrentFrameRDG();
    DynamicVoxelGIRenderer* renderer = StaticRenderer(64);
    const SceneUniformData lighting  = StaticLighting();
    uint64_t generation              = 20;
    for (float slope : {0.0f, 0.2f})
    {
        for (float offset : {0.1f, 0.5f, 0.9f, 1.0f, 1.1f})
        {
            ASSERT_TRUE(graph.Begin());
            HeapVector<OccupiedCell> cells;
            for (uint32_t x = 10; x < 15; ++x)
            {
                const float low  = 10.0f + offset + slope * (static_cast<float>(x) - 12.5f);
                const float high = low + slope;
                const uint32_t first =
                    static_cast<uint32_t>(std::floor(low)) - (low == std::floor(low) ? 1 : 0);
                for (uint32_t y = 10; y < 15; ++y)
                {
                    for (uint32_t z = first; z <= static_cast<uint32_t>(std::floor(high)); ++z)
                    {
                        cells.push_back({{x, y, z}, GI_STATIC});
                    }
                }
            }
            StaticFixture fixture = StaticInputs(graph, cells);
            GIHit hit{};
            hit.identity =
                glm::uvec4(GI_HIT, GI_STATIC, fixture.ids[0], GI_CELL_PRECISION | GI_SURFACE_VALID);
            hit.normal             = Vec4(0, 0, 1, 0);
            hit.diffuseReflectance = Vec4(0.25f, 0.5f, 1, 0);
            const uint32_t rays    = 64 * GI_FACE_COUNT * GI_FACE_RAYS;
            HeapVector<GIHit> responses(rays + 64 * 8, hit);
            for (uint32_t index = rays; index < responses.size(); ++index)
            {
                responses[index].identity = glm::uvec4(GI_MISS, 0, GI_INVALID_CELL, 0);
            }
            const DeterministicGIProvider provider =
                ReferenceStatic(graph, responses, ++generation);
            ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, true));
            HeapVector<IrradianceProbe> probes;
            const Vec3 normal = glm::normalize(Vec3(-slope, 0, 1));
            for (uint32_t x = 0; x < 17; ++x)
            {
                for (uint32_t y = 0; y < 17; ++y)
                {
                    const float px = 10.0f + 5.0f * (x + 0.5f) / 17.0f;
                    const float py = 10.0f + 5.0f * (y + 0.5f) / 17.0f;
                    probes.push_back(
                        {Vec4(px, py, 10.0f + offset + slope * (px - 12.5f), 0), Vec4(normal, 0)});
                }
            }
            const HeapVector<Vec4> values = ProbeStatic(graph, *renderer, probes);
            for (const Vec4& value : values)
            {
                EXPECT_EQ(value.w, 1);
                for (uint32_t c = 0; c < 3; ++c)
                {
                    EXPECT_NEAR(value[c], hit.diffuseReflectance[c] * glm::pi<float>(), 0.002f);
                }
            }
        }
    }
}

TEST_P(DynamicVoxelGIIntegrationTest, UnlitHiddenBlockerPreventsALitSenderBehindIt)
{
    RenderGraph& graph             = *device->GetCurrentFrameRDG();
    HeapVector<OccupiedCell> cells = {{{10, 10, 14}, GI_STATIC}, {{10, 10, 10}, GI_STATIC}};
    for (uint32_t y = 9; y <= 11; ++y)
    {
        cells.push_back({{13, y, 13}, GI_STATIC});
    }
    cells.push_back({{10, 10, 12}, GI_STATIC});
    DynamicVoxelGIRenderer* renderer   = StaticRenderer(static_cast<uint32_t>(cells.size()));
    SceneUniformData lighting          = StaticLighting();
    lighting.lights[0].directionType.w = 1;
    lighting.lights[0].positionRange   = Vec4(20.5f, 10.5f, 15.5f, 64);
    float blocked                      = 0;
    for (uint32_t state = 0; state < 2; ++state)
    {
        ASSERT_TRUE(graph.Begin());
        if (state == 1)
        {
            cells.pop_back();
        }
        StaticFixture fixture = StaticInputs(graph, cells);
        fixture.inputs.listGeneration = state + 1;
        VoxelDDAProvider provider;
        ASSERT_TRUE(provider.Prepare(fixture.staticVoxels, fixture.dynamicVoxels,
                                     fixture.inputs.grid, state + 1));
        ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, provider, lighting, 1, true));
        const StaticReadback output = CaptureStatic(graph, *renderer);
        ASSERT_EQ(output.status.z, 0);
        EXPECT_EQ(output.masks[StaticID({10, 10, 10})],
                  1); // Lit sender, outside the rendered receiver tuple.
        const float incoming = output.faces[2 * (5 * StaticCells + StaticID({10, 10, 14}))].z;
        if (state == 0)
        {
            EXPECT_EQ(output.masks[StaticID({10, 10, 12})],
                      0); // Hidden occluder shades this blocker.
            blocked = incoming;
        }
        else
        {
            EXPECT_GT(incoming, blocked + 1e-5f);
        }
    }
}

TEST_P(DynamicVoxelGIIntegrationTest, StaticDDAAndDecodedProviderUseIdenticalGatherAndComposition)
{
    CheckProviderSubstitution(GITemporalMode::eOff, false);
}
