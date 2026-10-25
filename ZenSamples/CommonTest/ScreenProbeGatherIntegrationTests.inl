// Deliberately headless: six probes and at most 17x9 receiver texels per dispatch.
struct ProbeFixtureRecord
{
    Vec4 position{0};
    Vec4 normal{0};
    Vec4 screen{0};
    Vec4 sky[4]{};
    Vec4 bounce[4]{};
    Vec4 skyAnchor{0};
    Vec4 bounceAnchor{0};
};
static_assert(sizeof(ProbeFixtureRecord) == 208);

class ScreenProbeGatherIntegrationTest : public ConeVoxelGIIntegrationTest
{
protected:
    static constexpr uint32_t Width  = 17;
    static constexpr uint32_t Height = 9;
    static constexpr uint32_t Pixels = Width * Height;
    // A 3x2 uniform grid at spacing 8, two adaptive rows (ZEN_PROBE_ADAPTIVE_ROW_RATIO), and 3x2
    // subtiles of ZEN_PROBE_SUBTILE.
    static constexpr uint32_t Slots        = 12;
    static constexpr uint32_t SubtileCount = 6;
    RHISampler*               sampler{nullptr};
    RHITexture*               receiver[6]{};
    HybridGIUniformData       hybrid;
    ScreenProbeUniformData    probe;
    SceneUniformData          scene;

    void SetUp() override
    {
        ConeVoxelGIIntegrationTest::SetUp();
        if (!HasFatalFailure())
        {
            ShaderProgramManager::GetInstance().StoreProgram(
                ZEN_NEW() ComputeFileSP(device, "ProbeTextureCheckSP", "VoxelGI/Calibration/probe_texture_check.comp.spv"));
            sampler           = device->CreateSampler({});
            hybrid.sampling   = glm::uvec4(0, 1, 32, 1);
            hybrid.rejection  = Vec4(.01f, 1, 1, 0);
            hybrid.bounce     = glm::uvec4(1, 1, 0, 0);
            hybrid.reflection = glm::uvec4(1, 1, 0, 0);
            probe.grid        = glm::uvec4(3, 2, 8, 1);
            probe.parameters  = Vec4(.01f, 32, 0, 0);
            // The history holds as many slots as the current frame.
            probe.adaptive  = glm::uvec4(3, 2, 2, Slots);
            scene.viewPos   = Vec4(0, 0, 3, 0);
            scene.lightInfo = Vec4(0, 0, 0, 1);
        }
    }

    RHITexture* Texture(DataFormat format, uint32_t width = Width, uint32_t height = Height)
    {
        RHITextureCreateInfo info;
        info.type   = RHITextureType::e2D;
        info.width  = width;
        info.height = height;
        info.depth  = 1;
        info.format = format;
        info.usageFlags.SetFlags(RHITextureUsageFlagBits::eSampled, RHITextureUsageFlagBits::eStorage,
                                 RHITextureUsageFlagBits::eTransferDst);
        RHITexture* texture = device->CreateTexture(info);
        EXPECT_NE(texture, nullptr);
        textures.push_back(texture);
        return texture;
    }

    template <class T> RHITexture* Upload(RenderGraph&         graph,
                                          DataFormat           format,
                                          const HeapVector<T>& values,
                                          uint32_t             width  = Width,
                                          uint32_t             height = Height)
    {
        RHITexture* texture = Texture(format, width, height);
        RHIBuffer*  upload =
            Buffer(static_cast<uint32_t>(values.size() * sizeof(T)), RHIBufferAllocateType::eCPUWrite, values.data());
        graph.GetResourceManager()->ImportHostWrittenBuffer(upload);
        RHIBufferTextureCopyRegion region;
        region.textureSize = Vec3i(width, height, 1);
        region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
        graph.AddTransferPass("UploadProbeFixture").CopyBufferToTexture(upload, texture, region);
        return texture;
    }

    void Receivers(RenderGraph& graph)
    {
        HeapVector<float>      depth(Pixels, .5f);
        HeapVector<Vec4>       positions(Pixels, Vec4(0, 0, 0, 1));
        HeapVector<glm::uvec2> surface(Pixels, glm::uvec2(0x80008000u, 1));
        depth[0]                = 1;
        surface[1].y            = 0;
        positions[Pixels - 1].z = 1;
        surface[Pixels - 1].y   = 99;
        receiver[0]             = Upload(graph, DataFormat::eR32SFloat, depth);
        receiver[1] = Upload(graph, DataFormat::eR32G32B32A32SFloat, HeapVector<Vec4>(Pixels, Vec4(.5f, .5f, 0, 0)));
        receiver[2] = Upload(graph, DataFormat::eR32G32B32A32SFloat, HeapVector<Vec4>(Pixels, Vec4(0, .5f, 0, 0)));
        receiver[3] = Upload(graph, DataFormat::eR32G32UInt, surface);
        receiver[4] = Upload(graph, DataFormat::eR32G32B32A32SFloat, HeapVector<Vec4>(Pixels, Vec4(0, 0, 1, 0)));
        receiver[5] = Upload(graph, DataFormat::eR32G32B32A32SFloat, positions);
    }

    void Inputs(RDGComputePassDesc& pass)
    {
        const char* const names[] = {"receiverDepth",   "receiverNormal", "receiverRoughness",
                                     "receiverSurface", "receiverMotion", "receiverPosition"};
        pass.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
        pass.BindValue("uHybridData", hybrid);
        pass.BindValue("uProbeData", probe);
        pass.BindValue("uSceneData", scene);
        for (uint32_t index = 0; index < 6; ++index)
        {
            pass.BindSampledTexture(names[index], sampler, receiver[index]->GetDefaultView());
        }
    }

    void Dispatch(RenderGraph& graph, RDGComputePassDesc&& pass, uint32_t width, uint32_t height)
    {
        const glm::uvec2 groups((width + 7) / 8, (height + 7) / 8);
        graph.AddComputePass(std::move(pass)).RecordPassCommands([groups](RDGPassCmdEncoder& encoder) {
            encoder.Dispatch(groups.x, groups.y, 1);
        });
    }

    // probe_allocate_count.comp, then probe_allocate.comp, over every subtile of probe.adaptive.
    void Allocate(RenderGraph& graph, RHIBuffer* candidates, RHIBuffer* placements, RHIBuffer* map, RHIBuffer* counters)
    {
        const uint32_t groups = (probe.adaptive.x * probe.adaptive.y + ZEN_PROBE_ALLOCATE_GROUP - 1) / ZEN_PROBE_ALLOCATE_GROUP;
        RHIBuffer*     blocks = Buffer(groups * sizeof(glm::uvec4), RHIBufferAllocateType::eGPU);
        RDGComputePassDesc count;
        count.SetShaderProgramName("ProbeAllocateCountSP");
        Inputs(count);
        count.BindStorageBuffer("ProbeSubtileCandidates", candidates);
        count.BindStorageBuffer("ProbeAllocationBlocks", blocks, RDGContentGuarantee::eFullWrite);
        graph.AddComputePass(std::move(count)).RecordPassCommands([groups](RDGPassCmdEncoder& encoder) {
            encoder.Dispatch(groups, 1, 1);
        });
        RDGComputePassDesc allocate;
        allocate.SetShaderProgramName("ProbeAllocateSP");
        Inputs(allocate);
        allocate.BindStorageBuffer("ProbeSubtileCandidates", candidates);
        allocate.BindStorageBuffer("ProbeAllocationBlocks", blocks);
        allocate.BindStorageBuffer("ProbeAdaptivePlacements", placements, RDGContentGuarantee::eProducedElements);
        allocate.BindStorageBuffer("ProbeSubtiles", map, RDGContentGuarantee::eFullWrite);
        allocate.BindStorageBuffer("ProbeCounters", counters, RDGContentGuarantee::eFullWrite);
        graph.AddComputePass(std::move(allocate)).RecordPassCommands([groups](RDGPassCmdEncoder& encoder) {
            encoder.Dispatch(groups, 1, 1);
        });
    }

    // probe_claim.comp, then probe_place.comp, over the 3x2 uniform grid.
    void Place(RenderGraph& graph, RHIBuffer* placements, RHIBuffer* history)
    {
        RHIBuffer*         claims = Buffer(6 * sizeof(uint32_t), RHIBufferAllocateType::eGPU);
        RDGComputePassDesc claim;
        claim.SetShaderProgramName("ProbeClaimSP");
        Inputs(claim);
        claim.BindStorageBuffer("ProbeHistory", history);
        claim.BindStorageBuffer("ProbeClaims", claims, RDGContentGuarantee::eFullWrite);
        Dispatch(graph, std::move(claim), 3, 2);
        RDGComputePassDesc place;
        place.SetShaderProgramName("ProbePlaceSP");
        Inputs(place);
        place.BindStorageBuffer("ProbePlacements", placements, RDGContentGuarantee::eFullWrite);
        place.BindStorageBuffer("ProbeHistory", history);
        place.BindStorageBuffer("ProbeClaims", claims);
        Dispatch(graph, std::move(place), 3, 2);
    }

    RHIBuffer* ReadBuffer(RenderGraph& graph, RHIBuffer* source)
    {
        RHIBuffer* readback = Buffer(static_cast<uint32_t>(source->GetRequiredSize()), RHIBufferAllocateType::eCPURead);
        graph.AddTransferPass("ReadProbeFixture").CopyBuffer(source, readback, {0, 0, source->GetRequiredSize()}).NeverCull();
        return readback;
    }

    RHIBuffer* ReadColor(RenderGraph& graph, RHITexture* source)
    {
        RHIBuffer*         output = Buffer(Width * Height * sizeof(Vec4), RHIBufferAllocateType::eGPU);
        RDGComputePassDesc pass;
        pass.SetShaderProgramName("ProbeTextureCheckSP");
        pass.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
        pass.BindSampledTexture("source", sampler, source->GetDefaultView());
        pass.BindStorageBuffer("Results", output, RDGContentGuarantee::eFullWrite);
        Dispatch(graph, std::move(pass), Width, Height);
        return ReadBuffer(graph, output);
    }

    void Execute(RenderGraph& graph)
    {
        ASSERT_TRUE(graph.End()) << graph.GetResult().message;
        ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
        device->FlushRHIThread();
        device->WaitForIdle();
    }

    static void ExpectColor(const Vec4& actual, const Vec4& expected)
    {
        for (uint32_t component = 0; component < 4; ++component)
        {
            EXPECT_NEAR(actual[component], expected[component], .002f);
        }
    }

    static ProbeFixtureRecord Record(uint32_t index, const Vec4& sky, const Vec4& bounce, float history)
    {
        ProbeFixtureRecord result;
        result.position             = Vec4(0, 0, 0, 1);
        result.normal               = Vec4(0, 0, 1, history);
        const uint32_t identity     = 1;
        float          identityBits = 0;
        std::memcpy(&identityBits, &identity, sizeof(identity));
        result.screen =
            Vec4(std::min(4u + index % 3 * 8u, Width - 1), std::min(4u + index / 3 * 8u, Height - 1), identityBits, history);
        // Analytic SH for constant incident radiance on the +Z hemisphere.
        // Its Lambert convolution divided by pi equals the incident color.
        result.sky[0]    = sky * (2.0f * 3.14159265359f * .2820947918f);
        result.sky[3]    = sky * (3.14159265359f * .4886025119f);
        result.bounce[0] = bounce * (2.0f * 3.14159265359f * .2820947918f);
        result.bounce[3] = bounce * (3.14159265359f * .4886025119f);
        // Exact irradiance / pi along +Z for the same constant radiance.
        result.skyAnchor    = sky;
        result.bounceAnchor = bounce;
        return result;
    }
};

// A probe placement kept from the previous frame keeps four times the configured sky history (here
// 128 frames); a new probe, moving or still, and bounce in every case, keep the configured 32.
TEST_P(ScreenProbeGatherIntegrationTest, KeptProbeSkyHistoryGrowsLonger)
{
    for (uint32_t scenario = 0; scenario < 3; ++scenario)
    {
        SCOPED_TRACE(scenario);
        RenderGraph graph("TinyProbeSkyHistory");
        ASSERT_TRUE(graph.Begin());
        Receivers(graph);
        if (scenario == 1)
        {
            // Every receiver moved by about four pixels since the previous frame.
            receiver[4] = Upload(graph, DataFormat::eR32G32B32A32SFloat, HeapVector<Vec4>(Pixels, Vec4(.5f, 0, 1, 0)));
        }
        HeapVector<ProbeFixtureRecord> current(Slots), previous(Slots);
        for (uint32_t index = 0; index < 6; ++index)
        {
            current[index]  = Record(index, Vec4(1), Vec4(1), 1);
            previous[index] = Record(index, Vec4(1), Vec4(1), 100);
        }
        if (scenario == 0)
        {
            // probe_place.comp kept slot 0: a negative normal.w names it.
            current[0].normal.w = -1;
        }
        const uint32_t             recordBytes = Slots * sizeof(ProbeFixtureRecord);
        const HeapVector<uint32_t> invalid(SubtileCount * ZEN_PROBE_ADAPTIVE_PER_SUBTILE, ZEN_PROBE_INVALID);
        const HeapVector<uint32_t> noAdaptive(8, 0u);
        RHIBuffer*                 input    = Buffer(recordBytes, RHIBufferAllocateType::eCPUWrite, current.data());
        RHIBuffer*                 history  = Buffer(recordBytes, RHIBufferAllocateType::eCPUWrite, previous.data());
        RHIBuffer*                 temporal = Buffer(recordBytes, RHIBufferAllocateType::eGPU);
        RHIBuffer*                 subtiles = Buffer(SubtileCount * ZEN_PROBE_ADAPTIVE_PER_SUBTILE * sizeof(uint32_t),
                                                     RHIBufferAllocateType::eCPUWrite, invalid.data());
        RHIBuffer*                 counters = Buffer(8 * sizeof(uint32_t), RHIBufferAllocateType::eCPUWrite, noAdaptive.data());
        for (RHIBuffer* buffer : {input, history, subtiles, counters})
        {
            graph.GetResourceManager()->ImportHostWrittenBuffer(buffer);
        }
        RDGComputePassDesc accumulate;
        accumulate.SetShaderProgramName("ProbeTemporalSP");
        Inputs(accumulate);
        accumulate.BindStorageBuffer("ProbeInput", input);
        accumulate.BindStorageBuffer("ProbeHistory", history);
        accumulate.BindStorageBuffer("ProbeOutput", temporal, RDGContentGuarantee::eFullWrite);
        accumulate.BindStorageBuffer("ProbeSubtilesPrevious", subtiles);
        accumulate.BindStorageBuffer("ProbeCounters", counters);
        Dispatch(graph, std::move(accumulate), 3, 3);
        RHIBuffer* readback = ReadBuffer(graph, temporal);
        Execute(graph);
        ASSERT_FALSE(HasFatalFailure());
        const HeapVector<ProbeFixtureRecord> accumulated = ReadStaticBuffer<ProbeFixtureRecord>(readback);
        EXPECT_EQ(accumulated[0].normal.w, scenario == 0 ? 101.0f : 32.0f);
        EXPECT_EQ(accumulated[0].screen.w, 32.0f);
    }
}

// A sample averaged over four ray batches (screen.w) counts as four frames of sky and bounce
// history; without valid history it starts at four.
TEST_P(ScreenProbeGatherIntegrationTest, ProbeRayBatchesCountAsHistory)
{
    for (uint32_t scenario = 0; scenario < 2; ++scenario)
    {
        SCOPED_TRACE(scenario);
        RenderGraph graph("TinyProbeBatches");
        ASSERT_TRUE(graph.Begin());
        Receivers(graph);
        probe.grid.w = scenario;
        HeapVector<ProbeFixtureRecord> current(Slots), previous(Slots);
        for (uint32_t index = 0; index < 6; ++index)
        {
            current[index]  = Record(index, Vec4(3), Vec4(3), 4);
            previous[index] = Record(index, Vec4(1), Vec4(1), 4);
        }
        // probe_place.comp kept slot 0.
        current[0].normal.w                    = -1;
        const uint32_t             recordBytes = Slots * sizeof(ProbeFixtureRecord);
        const HeapVector<uint32_t> invalid(SubtileCount * ZEN_PROBE_ADAPTIVE_PER_SUBTILE, ZEN_PROBE_INVALID);
        const HeapVector<uint32_t> noAdaptive(8, 0u);
        RHIBuffer*                 input    = Buffer(recordBytes, RHIBufferAllocateType::eCPUWrite, current.data());
        RHIBuffer*                 history  = Buffer(recordBytes, RHIBufferAllocateType::eCPUWrite, previous.data());
        RHIBuffer*                 temporal = Buffer(recordBytes, RHIBufferAllocateType::eGPU);
        RHIBuffer*                 subtiles = Buffer(SubtileCount * ZEN_PROBE_ADAPTIVE_PER_SUBTILE * sizeof(uint32_t),
                                                     RHIBufferAllocateType::eCPUWrite, invalid.data());
        RHIBuffer*                 counters = Buffer(8 * sizeof(uint32_t), RHIBufferAllocateType::eCPUWrite, noAdaptive.data());
        for (RHIBuffer* buffer : {input, history, subtiles, counters})
        {
            graph.GetResourceManager()->ImportHostWrittenBuffer(buffer);
        }
        RDGComputePassDesc accumulate;
        accumulate.SetShaderProgramName("ProbeTemporalSP");
        Inputs(accumulate);
        accumulate.BindStorageBuffer("ProbeInput", input);
        accumulate.BindStorageBuffer("ProbeHistory", history);
        accumulate.BindStorageBuffer("ProbeOutput", temporal, RDGContentGuarantee::eFullWrite);
        accumulate.BindStorageBuffer("ProbeSubtilesPrevious", subtiles);
        accumulate.BindStorageBuffer("ProbeCounters", counters);
        Dispatch(graph, std::move(accumulate), 3, 3);
        RHIBuffer* readback = ReadBuffer(graph, temporal);
        Execute(graph);
        ASSERT_FALSE(HasFatalFailure());
        const HeapVector<ProbeFixtureRecord> accumulated = ReadStaticBuffer<ProbeFixtureRecord>(readback);
        // Four frames of history plus four batches: the new sample weighs one half.
        const float expectedLength = scenario == 1 ? 8.0f : 4.0f;
        const Vec4  expectedSignal = scenario == 1 ? Vec4(2) : Vec4(3);
        EXPECT_FLOAT_EQ(accumulated[0].normal.w, expectedLength);
        EXPECT_FLOAT_EQ(accumulated[0].screen.w, expectedLength);
        ExpectColor(accumulated[0].skyAnchor, expectedSignal);
        ExpectColor(accumulated[0].bounceAnchor, expectedSignal);
    }
}

TEST_P(ScreenProbeGatherIntegrationTest, PlacementKeepsPreviousProbesThatStillProjectOntoTheirSurfaces)
{
    // Receivers lie where an identity projection puts them, offset by the reconstruction origin.
    // Previous slots 1 and 4 now project into tile (0, 0); slot 4's longer history wins, and slot 1
    // goes to the free tile whose centre is nearest it, (0, 1). Slot 3 projects into tile (1, 0),
    // but the receiver there is another surface, so that tile places a new probe.
    for (const float viewHistory : {100.0f, 2.0f})
    {
        SCOPED_TRACE(viewHistory);
        RenderGraph graph("TinyProbeKeep");
        ASSERT_TRUE(graph.Begin());
        const Vec3 origin(3, -2, 5);
        hybrid.reconstruction.inverseViewProjection = Mat4(1.0f);
        hybrid.reconstruction.worldOrigin           = Vec4(origin, 0);
        probe.parameters.w                          = viewHistory;
        Receivers(graph);
        HeapVector<Vec4> positions(Pixels);
        for (uint32_t index = 0; index < Pixels; ++index)
        {
            const Vec3 ndc((index % Width + .5f) / Width * 2 - 1, (index / Width + .5f) / Height * 2 - 1, .5f);
            positions[index] = Vec4(ndc + origin, 1);
        }
        receiver[5] = Upload(graph, DataFormat::eR32G32B32A32SFloat, positions);
        HeapVector<ProbeFixtureRecord> previous(Slots);
        const uint32_t                 keptPixels[][2] = {{1, 5 * Width + 5}, {3, 2 * Width + 10}, {4, 3 * Width + 2}};
        const float                    histories[]     = {10, 100, 50};
        for (uint32_t entry = 0; entry < 3; ++entry)
        {
            const uint32_t slot     = keptPixels[entry][0];
            previous[slot]          = Record(slot, Vec4(1), Vec4(1), histories[entry]);
            previous[slot].position = positions[keptPixels[entry][1]];
        }
        const uint32_t otherSurface = 7;
        std::memcpy(&previous[3].screen.z, &otherSurface, sizeof(otherSurface));
        RHIBuffer* history    = Buffer(Slots * sizeof(ProbeFixtureRecord), RHIBufferAllocateType::eCPUWrite, previous.data());
        RHIBuffer* placements = Buffer(6 * 3 * sizeof(Vec4), RHIBufferAllocateType::eGPU);
        graph.GetResourceManager()->ImportHostWrittenBuffer(history);
        Place(graph, placements, history);
        RHIBuffer* readback = ReadBuffer(graph, placements);
        Execute(graph);
        ASSERT_FALSE(HasFatalFailure());
        const HeapVector<Vec4> placed = ReadStaticBuffer<Vec4>(readback);
        // Tile (0, 0): slot 4 at its exact previous position, now at pixel (2, 3).
        EXPECT_EQ(placed[0], Vec4(Vec3(previous[4].position), 1));
        EXPECT_EQ(placed[1], Vec4(0, 0, 1, -5));
        EXPECT_EQ(Vec2(placed[2]), Vec2(2, 3));
        // Tile (0, 1): slot 1, displaced from tile (0, 0), still at pixel (5, 5).
        EXPECT_EQ(placed[9], Vec4(Vec3(previous[1].position), 1));
        EXPECT_EQ(placed[10], Vec4(0, 0, 1, -2));
        EXPECT_EQ(Vec2(placed[11]), Vec2(5, 5));
        // Tiles (1, 0) and (1, 1): new receivers.
        EXPECT_EQ(placed[3].w, 1.0f);
        EXPECT_EQ(placed[4].w, 1.0f);
        EXPECT_EQ(placed[12].w, 1.0f);
        EXPECT_EQ(placed[13].w, 1.0f);
        // Ray batches by history (ProbeRayBatches), once the view's history is 100 frames long: 50
        // frames trace one, 10 two, a new probe four. Two frames after the history restarted, no probe
        // is clearly younger than the view and each traces one.
        const bool settled = viewHistory > 50.0f;
        EXPECT_EQ(placed[2].w, 1.0f);
        EXPECT_EQ(placed[11].w, settled ? 2.0f : 1.0f);
        EXPECT_EQ(placed[5].w, settled ? 4.0f : 1.0f);
    }
}

TEST_P(ScreenProbeGatherIntegrationTest, ProbeSHReconstructionNormalizesAndFallsBackForUncoveredReceivers)
{
    const Vec4 currentSky(.25f, .5f, 1, 1), previousSky(.75f, 1, 1.5f, 1);
    const Vec4 currentBounce(.1f, .2f, .4f, 0), previousBounce(.3f, .4f, .6f, 0);
    for (uint32_t scenario = 0; scenario < 4; ++scenario)
    {
        SCOPED_TRACE(scenario);
        RenderGraph graph("TinyProbeReconstruction");
        ASSERT_TRUE(graph.Begin());
        Receivers(graph);
        probe.grid.w    = scenario == 1 ? 0u : 1u;
        hybrid.bounce.w = scenario == 3 ? 1u : 0u;
        // Adaptive slots 6-8 stay empty; every subtile map entry is invalid.
        HeapVector<ProbeFixtureRecord> current(Slots), previous(Slots);
        for (uint32_t index = 0; index < 6; ++index)
        {
            current[index]  = Record(index, currentSky, currentBounce, 1);
            previous[index] = Record(index, previousSky, previousBounce, 3);
            if (scenario == 2)
            {
                previous[index].screen.z = 0;
            }
        }
        current[5]                 = {};
        previous[5]                = {};
        const uint32_t recordBytes = Slots * sizeof(ProbeFixtureRecord);
        RHIBuffer*     input       = Buffer(recordBytes, RHIBufferAllocateType::eCPUWrite, current.data());
        RHIBuffer*     history     = Buffer(recordBytes, RHIBufferAllocateType::eCPUWrite, previous.data());
        RHIBuffer*     temporal    = Buffer(recordBytes, RHIBufferAllocateType::eGPU);
        RHIBuffer*     filtered    = Buffer(recordBytes, RHIBufferAllocateType::eGPU);
        RHIBuffer*     cleared     = Buffer(recordBytes, RHIBufferAllocateType::eGPU);
        RHIBuffer*     clearedMap =
            Buffer(SubtileCount * ZEN_PROBE_ADAPTIVE_PER_SUBTILE * sizeof(uint32_t), RHIBufferAllocateType::eGPU);
        const HeapVector<uint32_t> invalid(SubtileCount * ZEN_PROBE_ADAPTIVE_PER_SUBTILE, ZEN_PROBE_INVALID);
        RHIBuffer*                 subtiles = Buffer(SubtileCount * ZEN_PROBE_ADAPTIVE_PER_SUBTILE * sizeof(uint32_t),
                                                     RHIBufferAllocateType::eCPUWrite, invalid.data());
        // probe_allocate.comp's counters: no adaptive probe this frame.
        const HeapVector<uint32_t> noAdaptive(8, 0u);
        RHIBuffer*                 counters = Buffer(8 * sizeof(uint32_t), RHIBufferAllocateType::eCPUWrite, noAdaptive.data());
        graph.GetResourceManager()->ImportHostWrittenBuffer(input);
        graph.GetResourceManager()->ImportHostWrittenBuffer(history);
        graph.GetResourceManager()->ImportHostWrittenBuffer(subtiles);
        graph.GetResourceManager()->ImportHostWrittenBuffer(counters);
        RDGComputePassDesc clear;
        clear.SetShaderProgramName("ProbeClearSP");
        Inputs(clear);
        clear.BindStorageBuffer("ProbeOutput", cleared, RDGContentGuarantee::eFullWrite);
        clear.BindStorageBuffer("ProbeSubtiles", clearedMap, RDGContentGuarantee::eFullWrite);
        graph.AddComputePass(std::move(clear)).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
            encoder.Dispatch(1, 1, 1);
        });
        RHIBuffer*         clearReadback    = ReadBuffer(graph, cleared);
        RHIBuffer*         clearMapReadback = ReadBuffer(graph, clearedMap);
        RDGComputePassDesc accumulate;
        accumulate.SetShaderProgramName("ProbeTemporalSP");
        Inputs(accumulate);
        accumulate.BindStorageBuffer("ProbeInput", input);
        accumulate.BindStorageBuffer("ProbeHistory", history);
        accumulate.BindStorageBuffer("ProbeOutput", temporal, RDGContentGuarantee::eFullWrite);
        accumulate.BindStorageBuffer("ProbeSubtilesPrevious", subtiles);
        accumulate.BindStorageBuffer("ProbeCounters", counters);
        Dispatch(graph, std::move(accumulate), 3, 3);
        RHIBuffer*         temporalReadback = ReadBuffer(graph, temporal);
        RDGComputePassDesc filter;
        filter.SetShaderProgramName("ProbeFilterSP");
        Inputs(filter);
        filter.BindStorageBuffer("ProbeInput", temporal);
        filter.BindStorageBuffer("ProbeOutput", filtered, RDGContentGuarantee::eFullWrite);
        filter.BindStorageBuffer("ProbeSubtiles", subtiles);
        Dispatch(graph, std::move(filter), 3, 3);
        RHITexture*        sky    = Texture(DataFormat::eR16G16B16A16SFloat);
        RHITexture*        bounce = Texture(DataFormat::eR16G16B16A16SFloat);
        RDGComputePassDesc integrate;
        integrate.SetShaderProgramName("ProbeIntegrateSP");
        Inputs(integrate);
        integrate.BindStorageBuffer("ProbeInput", filtered);
        integrate.BindStorageBuffer("ProbeSubtiles", subtiles);
        integrate.BindStorageImage("probeRawSky", sky->GetDefaultView(), RDGContentGuarantee::eFullWrite);
        integrate.BindStorageImage("probeRawBounce", bounce->GetDefaultView(), RDGContentGuarantee::eFullWrite);
        Dispatch(graph, std::move(integrate), Width, Height);
        RHIBuffer* skyReadback    = ReadColor(graph, sky);
        RHIBuffer* bounceReadback = ReadColor(graph, bounce);
        Execute(graph);
        ASSERT_FALSE(HasFatalFailure());
        const HeapVector<Vec4> zero = ReadStaticBuffer<Vec4>(clearReadback);
        for (const Vec4& value : zero)
        {
            EXPECT_EQ(value, Vec4(0));
        }
        for (uint32_t value : ReadStaticBuffer<uint32_t>(clearMapReadback))
        {
            EXPECT_EQ(value, ZEN_PROBE_INVALID);
        }
        const HeapVector<ProbeFixtureRecord> accumulated = ReadStaticBuffer<ProbeFixtureRecord>(temporalReadback);
        // A new probe's history is a weighted blend of its neighbours', so allow rounding.
        EXPECT_FLOAT_EQ(accumulated[0].normal.w, scenario == 0 || scenario == 3 ? 4.0f : 1.0f);
        EXPECT_FLOAT_EQ(accumulated[0].screen.w, scenario == 0 || scenario == 3 ? 4.0f : 1.0f);
        EXPECT_EQ(accumulated[5].position.w, 0);
        EXPECT_EQ(accumulated[Slots - 1].position.w, 0);
        const HeapVector<Vec4> skyPixels    = ReadStaticBuffer<Vec4>(skyReadback);
        const HeapVector<Vec4> bouncePixels = ReadStaticBuffer<Vec4>(bounceReadback);
        const Vec4 expectedSky = scenario == 0 || scenario == 3 ? previousSky * .75f + currentSky * .25f : currentSky;
        const Vec4 expectedBounce =
            scenario == 0 || scenario == 3 ? previousBounce * .75f + currentBounce * .25f : currentBounce;
        for (uint32_t index = 0; index < Pixels; ++index)
        {
            SCOPED_TRACE(index);
            // Indices 0 and 1 are not receivers. The last receiver lies off every probe's plane and
            // takes the facing-probe fallback, which here carries the same signal.
            const bool uncovered = index < 2;
            ExpectColor(skyPixels[index], uncovered ? Vec4(0) : expectedSky);
            ExpectColor(bouncePixels[index], uncovered ? Vec4(0) : expectedBounce);
        }
    }
}

TEST_P(ScreenProbeGatherIntegrationTest, AdaptiveProbesCoverReceiversUniformProbesReject)
{
    const Vec4  uniformSky(.25f, .5f, 1, 1), adaptiveSky(1, .5f, .25f, 1);
    RenderGraph graph("TinyAdaptiveProbeCoverage");
    ASSERT_TRUE(graph.Begin());
    Receivers(graph);
    HeapVector<ProbeFixtureRecord> records(Slots);
    for (uint32_t index = 0; index < 6; ++index)
    {
        records[index] = Record(index, uniformSky, Vec4(0), 1);
    }
    // The last receiver sits one unit above the uniform probes' plane. Its subtile's adaptive
    // probe shares that plane, and the uniform receivers in turn reject the adaptive probe.
    records[6]            = Record(5, adaptiveSky, Vec4(0), 1);
    records[6].position.z = 1;
    records[6].screen.x   = Width - 1;
    records[6].screen.y   = Height - 1;
    std::memcpy(&records[6].screen.z, &records[0].screen.z, sizeof(float));
    records[5] = {};
    // Four entries per subtile; the probe sits in the last subtile's second entry.
    HeapVector<uint32_t> subtiles(SubtileCount * ZEN_PROBE_ADAPTIVE_PER_SUBTILE, ZEN_PROBE_INVALID);
    subtiles[((Height - 1) / 8 * 3 + (Width - 1) / 8) * ZEN_PROBE_ADAPTIVE_PER_SUBTILE + 1] = 6;
    RHIBuffer* input = Buffer(Slots * sizeof(ProbeFixtureRecord), RHIBufferAllocateType::eCPUWrite, records.data());
    RHIBuffer* map = Buffer(SubtileCount * ZEN_PROBE_ADAPTIVE_PER_SUBTILE * sizeof(uint32_t), RHIBufferAllocateType::eCPUWrite,
                            subtiles.data());
    graph.GetResourceManager()->ImportHostWrittenBuffer(input);
    graph.GetResourceManager()->ImportHostWrittenBuffer(map);
    RHITexture*        sky    = Texture(DataFormat::eR16G16B16A16SFloat);
    RHITexture*        bounce = Texture(DataFormat::eR16G16B16A16SFloat);
    RDGComputePassDesc integrate;
    integrate.SetShaderProgramName("ProbeIntegrateSP");
    Inputs(integrate);
    integrate.BindStorageBuffer("ProbeInput", input);
    integrate.BindStorageBuffer("ProbeSubtiles", map);
    integrate.BindStorageImage("probeRawSky", sky->GetDefaultView(), RDGContentGuarantee::eFullWrite);
    integrate.BindStorageImage("probeRawBounce", bounce->GetDefaultView(), RDGContentGuarantee::eFullWrite);
    Dispatch(graph, std::move(integrate), Width, Height);
    RHIBuffer* skyReadback = ReadColor(graph, sky);
    Execute(graph);
    ASSERT_FALSE(HasFatalFailure());
    const HeapVector<Vec4> skyPixels = ReadStaticBuffer<Vec4>(skyReadback);
    for (uint32_t index = 0; index < Pixels; ++index)
    {
        SCOPED_TRACE(index);
        ExpectColor(skyPixels[index], index < 2 ? Vec4(0) : index == Pixels - 1 ? adaptiveSky : uniformSky);
    }
}

TEST_P(ScreenProbeGatherIntegrationTest, AdaptivePlacementAddsAProbeForASecondSurfaceInATile)
{
    // Tile (0, 0) holds two receiver columns facing +Z and +X, and only background between them.
    // Whichever the uniform probe lands on, the other column has no compatible uniform probe. Two
    // pixels wide, that column has ZEN_PROBE_ADAPTIVE_MIN_UNCOVERED receivers and gets an adaptive
    // probe; one pixel wide, it has half as many and is left to the integration fallback.
    for (uint32_t columnWidth : {2u, 1u})
    {
        SCOPED_TRACE(columnWidth);
        RenderGraph graph("TinyAdaptivePlacement");
        ASSERT_TRUE(graph.Begin());
        HeapVector<float>      depth(Pixels, 1.0f);
        HeapVector<Vec4>       positions(Pixels, Vec4(0, 0, 0, 1));
        HeapVector<glm::uvec2> surface(Pixels, glm::uvec2(0x80008000u, 1));
        const glm::uvec2       facingX(glm::packUnorm2x16(glm::vec2(1, .5f)), 2);
        for (uint32_t y = 0; y < 8; ++y)
        {
            for (uint32_t x = 0; x < 8; ++x)
            {
                const uint32_t index = y * Width + x;
                if (x < columnWidth || x >= 8 - columnWidth)
                {
                    depth[index] = .5f;
                    // Each column is planar along its own normal.
                    positions[index] = x < 4 ? Vec4(x * .1f, y * .1f, 0, 1) : Vec4(.6f, y * .1f, (x - 6) * .1f, 1);
                    surface[index]   = x < 4 ? surface[index] : facingX;
                }
            }
        }
        receiver[0]           = Upload(graph, DataFormat::eR32SFloat, depth);
        receiver[1]           = Upload(graph, DataFormat::eR32G32B32A32SFloat, HeapVector<Vec4>(Pixels, Vec4(.5f, .5f, 0, 0)));
        receiver[2]           = Upload(graph, DataFormat::eR32G32B32A32SFloat, HeapVector<Vec4>(Pixels, Vec4(0, .5f, 0, 0)));
        receiver[3]           = Upload(graph, DataFormat::eR32G32UInt, surface);
        receiver[4]           = Upload(graph, DataFormat::eR32G32B32A32SFloat, HeapVector<Vec4>(Pixels, Vec4(0, 0, 1, 0)));
        receiver[5]           = Upload(graph, DataFormat::eR32G32B32A32SFloat, positions);
        RHIBuffer* placements = Buffer(6 * 3 * sizeof(Vec4), RHIBufferAllocateType::eGPU);
        RHIBuffer* candidates =
            Buffer(SubtileCount * ZEN_PROBE_ADAPTIVE_PER_SUBTILE * 3 * sizeof(Vec4), RHIBufferAllocateType::eGPU);
        RHIBuffer* appended = Buffer(6 * 3 * sizeof(Vec4), RHIBufferAllocateType::eGPU);
        RHIBuffer* counters = Buffer(8 * sizeof(uint32_t), RHIBufferAllocateType::eGPU);
        RHIBuffer* map = Buffer(SubtileCount * ZEN_PROBE_ADAPTIVE_PER_SUBTILE * sizeof(uint32_t), RHIBufferAllocateType::eGPU);
        // No previous probe to keep.
        const HeapVector<ProbeFixtureRecord> empty(Slots);
        const HeapVector<uint32_t>           invalid(SubtileCount * ZEN_PROBE_ADAPTIVE_PER_SUBTILE, ZEN_PROBE_INVALID);
        RHIBuffer* history     = Buffer(Slots * sizeof(ProbeFixtureRecord), RHIBufferAllocateType::eCPUWrite, empty.data());
        RHIBuffer* previousMap = Buffer(SubtileCount * ZEN_PROBE_ADAPTIVE_PER_SUBTILE * sizeof(uint32_t),
                                        RHIBufferAllocateType::eCPUWrite, invalid.data());
        graph.GetResourceManager()->ImportHostWrittenBuffer(history);
        graph.GetResourceManager()->ImportHostWrittenBuffer(previousMap);
        Place(graph, placements, history);
        RDGComputePassDesc adaptive;
        adaptive.SetShaderProgramName("ProbeAdaptiveSP");
        Inputs(adaptive);
        adaptive.BindStorageBuffer("ProbePlacements", placements);
        adaptive.BindStorageBuffer("ProbeSubtileCandidates", candidates, RDGContentGuarantee::eFullWrite);
        adaptive.BindStorageBuffer("ProbeHistory", history);
        adaptive.BindStorageBuffer("ProbeSubtilesPrevious", previousMap);
        graph.AddComputePass(std::move(adaptive)).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
            encoder.Dispatch(3, 2, 1);
        });
        Allocate(graph, candidates, appended, map, counters);
        RHIBuffer* placementReadback = ReadBuffer(graph, placements);
        RHIBuffer* appendedReadback  = ReadBuffer(graph, appended);
        RHIBuffer* counterReadback   = ReadBuffer(graph, counters);
        RHIBuffer* mapReadback       = ReadBuffer(graph, map);
        Execute(graph);
        ASSERT_FALSE(HasFatalFailure());
        const HeapVector<Vec4>     placed         = ReadStaticBuffer<Vec4>(placementReadback);
        const HeapVector<Vec4>     added          = ReadStaticBuffer<Vec4>(appendedReadback);
        const HeapVector<uint32_t> count          = ReadStaticBuffer<uint32_t>(counterReadback);
        const HeapVector<uint32_t> slots          = ReadStaticBuffer<uint32_t>(mapReadback);
        const uint32_t             adaptiveProbes = columnWidth == 2 ? 1u : 0u;
        // Only subtile (0, 0) has uncovered receivers, and its first adaptive probe covers all of
        // them. An allocated row follows the two uniform rows.
        EXPECT_EQ(count[0], adaptiveProbes);
        EXPECT_EQ(count[1], adaptiveProbes);
        EXPECT_EQ(glm::uvec4(count[4], count[5], count[6], count[7]), glm::uvec4(3, 2 + adaptiveProbes, 1, 0));
        EXPECT_EQ(slots[0], columnWidth == 2 ? 6u : ZEN_PROBE_INVALID);
        for (uint32_t index = 1; index < SubtileCount * ZEN_PROBE_ADAPTIVE_PER_SUBTILE; ++index)
        {
            EXPECT_EQ(slots[index], ZEN_PROBE_INVALID) << index;
        }
        if (columnWidth == 2)
        {
            // Uniform tile (0, 0) found a receiver. One probe faces +Z and the other +X.
            ASSERT_EQ(placed[0].w, 1.0f);
            const Vec4 uniformNormal = placed[1], adaptiveNormal = added[1];
            EXPECT_EQ(added[0].w, 1.0f);
            EXPECT_NEAR(std::abs(glm::dot(Vec3(uniformNormal), Vec3(adaptiveNormal))), 0.0f, 1e-3f);
            EXPECT_NEAR(std::abs(uniformNormal.z) + std::abs(adaptiveNormal.z), 1.0f, 1e-3f);
        }
    }
}

// Rounds of adaptive candidates the allocation test offers in a subtile: zero to ZEN_PROBE_ADAPTIVE_PER_SUBTILE.
uint32_t OfferedProbeRounds(uint32_t subtile)
{
    return (subtile * 7u + subtile / 5u) % (ZEN_PROBE_ADAPTIVE_PER_SUBTILE + 1u);
}

TEST_P(ScreenProbeGatherIntegrationTest, AllocationFollowsRoundOrderAcrossBlocksWhenThePoolRunsShort)
{
    // 600 subtiles span three allocation workgroups. Subtile s offers its first n(s) rounds; the
    // 450-probe pool runs out partway through the second round, inside the second workgroup.
    RenderGraph graph("TinyProbeAllocation");
    ASSERT_TRUE(graph.Begin());
    Receivers(graph);
    probe.grid                = glm::uvec4(10, 4, 8, 1);
    probe.adaptive            = glm::uvec4(300, 2, 45, 0);
    const uint32_t   subtiles = probe.adaptive.x * probe.adaptive.y;
    const uint32_t   uniforms = probe.grid.x * probe.grid.y;
    const uint32_t   capacity = probe.grid.x * probe.adaptive.z;
    HeapVector<Vec4> candidateValues(subtiles * ZEN_PROBE_ADAPTIVE_PER_SUBTILE * 3, Vec4(0));
    for (uint32_t subtile = 0; subtile < subtiles; ++subtile)
    {
        for (uint32_t round = 0; round < OfferedProbeRounds(subtile); ++round)
        {
            const uint32_t entry       = (subtile * ZEN_PROBE_ADAPTIVE_PER_SUBTILE + round) * 3;
            candidateValues[entry]     = Vec4(float(subtile), float(round), 0, 1);
            candidateValues[entry + 1] = Vec4(0, 0, 1, 1);
            candidateValues[entry + 2] = Vec4(float(subtile), 0, 0, 1);
        }
    }
    RHIBuffer* candidates = Buffer(static_cast<uint32_t>(candidateValues.size() * sizeof(Vec4)),
                                   RHIBufferAllocateType::eCPUWrite, candidateValues.data());
    graph.GetResourceManager()->ImportHostWrittenBuffer(candidates);
    RHIBuffer* placements = Buffer(capacity * 3 * sizeof(Vec4), RHIBufferAllocateType::eGPU);
    RHIBuffer* map        = Buffer(subtiles * ZEN_PROBE_ADAPTIVE_PER_SUBTILE * sizeof(uint32_t), RHIBufferAllocateType::eGPU);
    RHIBuffer* counters   = Buffer(8 * sizeof(uint32_t), RHIBufferAllocateType::eGPU);
    Allocate(graph, candidates, placements, map, counters);
    RHIBuffer* placementReadback = ReadBuffer(graph, placements);
    RHIBuffer* mapReadback       = ReadBuffer(graph, map);
    RHIBuffer* counterReadback   = ReadBuffer(graph, counters);
    Execute(graph);
    ASSERT_FALSE(HasFatalFailure());
    const HeapVector<Vec4>     placed = ReadStaticBuffer<Vec4>(placementReadback);
    const HeapVector<uint32_t> slots  = ReadStaticBuffer<uint32_t>(mapReadback);
    const HeapVector<uint32_t> count  = ReadStaticBuffer<uint32_t>(counterReadback);
    // Every subtile's first round, in subtile order, then every second round, and so on.
    HeapVector<uint32_t> expected(subtiles * ZEN_PROBE_ADAPTIVE_PER_SUBTILE, ZEN_PROBE_INVALID);
    uint32_t             requested = 0;
    for (uint32_t round = 0; round < ZEN_PROBE_ADAPTIVE_PER_SUBTILE; ++round)
    {
        for (uint32_t subtile = 0; subtile < subtiles; ++subtile)
        {
            if (round < OfferedProbeRounds(subtile))
            {
                if (requested < capacity)
                {
                    expected[subtile * ZEN_PROBE_ADAPTIVE_PER_SUBTILE + round] = uniforms + requested;
                    SCOPED_TRACE(requested);
                    EXPECT_EQ(placed[requested * 3], Vec4(float(subtile), float(round), 0, 1));
                }
                ++requested;
            }
        }
    }
    ASSERT_GT(requested, capacity);
    EXPECT_EQ(count[0], capacity);
    EXPECT_EQ(count[1], requested);
    EXPECT_EQ(glm::uvec4(count[4], count[5], count[6], count[7]),
              glm::uvec4(probe.grid.x, probe.grid.y + probe.adaptive.z, 1, 0));
    for (uint32_t index = 0; index < subtiles * ZEN_PROBE_ADAPTIVE_PER_SUBTILE; ++index)
    {
        EXPECT_EQ(slots[index], expected[index]) << index;
    }
}

TEST_P(ScreenProbeGatherIntegrationTest, SpecularResolvePreservesConstantSignalAndRejectsForeignSurface)
{
    RenderGraph graph("TinyProbeSpecularResolve");
    ASSERT_TRUE(graph.Begin());
    Receivers(graph);
    const uint32_t   halfWidth = (Width + 1) / 2, halfHeight = (Height + 1) / 2;
    const Vec4       signal(.5f, 1, 2, .25f);
    HeapVector<Vec4> guides(halfWidth * halfHeight);
    for (uint32_t y = 0; y < halfHeight; ++y)
    {
        for (uint32_t x = 0; x < halfWidth; ++x)
        {
            guides[y * halfWidth + x] = Vec4(x * 2, y * 2, 0, 0);
        }
    }
    guides.back() = Vec4(-1, -1, 0, 0);
    RHITexture* samples =
        Upload(graph, DataFormat::eR32G32B32A32SFloat, HeapVector<Vec4>(halfWidth * halfHeight, signal), halfWidth, halfHeight);
    RHITexture*        sources  = Upload(graph, DataFormat::eR32G32B32A32SFloat, guides, halfWidth, halfHeight);
    RHITexture*        resolved = Texture(DataFormat::eR16G16B16A16SFloat);
    RDGComputePassDesc pass;
    pass.SetShaderProgramName("ProbeSpecularResolveSP");
    Inputs(pass);
    pass.BindSampledTexture("rawProbeSpecular", sampler, samples->GetDefaultView());
    pass.BindSampledTexture("rawProbeSpecularGuide", sampler, sources->GetDefaultView());
    pass.BindStorageImage("probeRawSpecular", resolved->GetDefaultView(), RDGContentGuarantee::eFullWrite);
    Dispatch(graph, std::move(pass), Width, Height);
    RHIBuffer* readback = ReadColor(graph, resolved);
    Execute(graph);
    ASSERT_FALSE(HasFatalFailure());
    const HeapVector<Vec4> result = ReadStaticBuffer<Vec4>(readback);
    for (uint32_t index = 0; index < Pixels; ++index)
    {
        SCOPED_TRACE(index);
        // Indices 0 and 1 are not receivers. The last receiver has no compatible sample and carries
        // the no-sample marker.
        ExpectColor(result[index], index < 2 ? Vec4(0, 0, 0, 1) : index == Pixels - 1 ? Vec4(0, 0, 0, -1) : signal);
    }
}

TEST_P(ScreenProbeGatherIntegrationTest, SpecularFilterAveragesOnlyItsOwnSurface)
{
    // Every block traced a receiver of surface 1 at the origin, except block 0, which found none,
    // and the last block, on surface 99 one unit away: both keep their samples, and neither
    // reaches the blocks of surface 1.
    RenderGraph graph("TinyProbeSpecularFilter");
    ASSERT_TRUE(graph.Begin());
    Receivers(graph);
    const uint32_t         halfWidth = (Width + 1) / 2, halfHeight = (Height + 1) / 2;
    const uint32_t         blocks = halfWidth * halfHeight;
    const Vec4             signal(.5f, 1, 2, .25f), empty(8, 8, 8, 1), foreign(4, 4, 4, 0);
    HeapVector<Vec4>       samples(blocks, signal);
    HeapVector<glm::uvec4> surfaces(blocks, glm::uvec4(1, 0x80008000u, 0x80008000u, 0));
    HeapVector<Vec4>       places(blocks, Vec4(0, 0, 0, .5f));
    samples.front()             = empty;
    samples.back()              = foreign;
    surfaces.front()            = glm::uvec4(0);
    surfaces.back().x           = 99;
    places.back().z             = 1;
    RHITexture*        input    = Upload(graph, DataFormat::eR32G32B32A32SFloat, samples, halfWidth, halfHeight);
    RHITexture*        surface  = Upload(graph, DataFormat::eR32G32B32A32UInt, surfaces, halfWidth, halfHeight);
    RHITexture*        place    = Upload(graph, DataFormat::eR32G32B32A32SFloat, places, halfWidth, halfHeight);
    RHITexture*        filtered = Texture(DataFormat::eR16G16B16A16SFloat, halfWidth, halfHeight);
    RDGComputePassDesc pass;
    pass.SetShaderProgramName("ProbeSpecularFilter0SP");
    Inputs(pass);
    pass.BindSampledTexture("rawProbeSpecular", sampler, input->GetDefaultView());
    pass.BindSampledTexture("rawProbeSpecularSurface", sampler, surface->GetDefaultView());
    pass.BindSampledTexture("rawProbeSpecularPosition", sampler, place->GetDefaultView());
    pass.BindStorageImage("filteredProbeSpecular", filtered->GetDefaultView(), RDGContentGuarantee::eFullWrite);
    Dispatch(graph, std::move(pass), halfWidth, halfHeight);
    RHIBuffer* readback = ReadColor(graph, filtered);
    Execute(graph);
    ASSERT_FALSE(HasFatalFailure());
    const HeapVector<Vec4> result = ReadStaticBuffer<Vec4>(readback);
    for (uint32_t index = 0; index < blocks; ++index)
    {
        SCOPED_TRACE(index);
        ExpectColor(result[index], index == 0 ? empty : index == blocks - 1 ? foreign : signal);
    }
}

TEST_P(ScreenProbeGatherIntegrationTest, PixelTemporalUsesDiffuseSkyVisibilityWithoutSpecularSample)
{
    // No history and no resolved specular sample (alpha -1): visibility falls back to the
    // receiver's diffuse sky visibility, with zero history length.
    const char* const names[]   = {"Sky", "Bounce", "Specular", "Position", "Surface", "NormalRoughness"};
    const DataFormat  formats[] = {DataFormat::eR16G16B16A16SFloat, DataFormat::eR16G16B16A16SFloat,
                                   DataFormat::eR16G16B16A16SFloat, DataFormat::eR32G32B32A32SFloat,
                                   DataFormat::eR32G32UInt,         DataFormat::eR16G16B16A16SFloat};
    const Vec4        current[] = {Vec4(.25f, .5f, 1, .75f), Vec4(.1f, .2f, .4f, 1), Vec4(0, 0, 0, -1)};
    RenderGraph       graph("TinyProbePixelFallback");
    ASSERT_TRUE(graph.Begin());
    Receivers(graph);
    probe.grid.w = 0;
    RHITexture*        next[6]{};
    RDGComputePassDesc pass;
    pass.SetShaderProgramName("ProbePixelTemporalSP");
    Inputs(pass);
    for (uint32_t signal = 0; signal < 6; ++signal)
    {
        RHITexture* old = Texture(formats[signal]);
        next[signal]    = Texture(formats[signal]);
        graph.AddTransferPass("ClearProbeFixture").ClearTexture(old, Color(0));
        pass.BindSampledTexture(NameID(fmt::format("previousProbe{}", names[signal])), sampler, old->GetDefaultView());
        pass.BindStorageImage(NameID(fmt::format("nextProbe{}", names[signal])), next[signal]->GetDefaultView(),
                              RDGContentGuarantee::eFullWrite);
        if (signal < 3)
        {
            RHITexture* raw = Upload(graph, DataFormat::eR32G32B32A32SFloat, HeapVector<Vec4>(Pixels, current[signal]));
            pass.BindSampledTexture(NameID(fmt::format("probeRaw{}", names[signal])), sampler, raw->GetDefaultView());
        }
    }
    Dispatch(graph, std::move(pass), Width, Height);
    RHIBuffer* specular        = ReadColor(graph, next[2]);
    RHIBuffer* normalRoughness = ReadColor(graph, next[5]);
    Execute(graph);
    ASSERT_FALSE(HasFatalFailure());
    const HeapVector<Vec4> visibility = ReadStaticBuffer<Vec4>(specular);
    const HeapVector<Vec4> lengths    = ReadStaticBuffer<Vec4>(normalRoughness);
    for (uint32_t index = 2; index < Pixels; ++index)
    {
        SCOPED_TRACE(index);
        ExpectColor(visibility[index], Vec4(0, 0, 0, .75f));
        EXPECT_EQ(lengths[index].w, 0.0f);
    }
}

TEST_P(ScreenProbeGatherIntegrationTest, PixelTemporalChecksReceiverHistoryAndSignalResets)
{
    const char* const names[]    = {"Sky", "Bounce", "Specular", "Position", "Surface", "NormalRoughness"};
    const DataFormat  formats[]  = {DataFormat::eR16G16B16A16SFloat, DataFormat::eR16G16B16A16SFloat,
                                    DataFormat::eR16G16B16A16SFloat, DataFormat::eR32G32B32A32SFloat,
                                    DataFormat::eR32G32UInt,         DataFormat::eR16G16B16A16SFloat};
    const Vec4        current[]  = {Vec4(.25f, .5f, 1, .75f), Vec4(.1f, .2f, .4f, 1), Vec4(.5f, 1, 2, .25f)};
    const Vec4        previous[] = {Vec4(.75f, 1, 1.5f, .25f), Vec4(.6f, .8f, 1, 3), Vec4(1, 2, 3, .75f)};
    for (uint32_t scenario = 0; scenario < 4; ++scenario)
    {
        SCOPED_TRACE(scenario);
        RenderGraph graph("TinyProbePixelHistory");
        ASSERT_TRUE(graph.Begin());
        Receivers(graph);
        probe.grid.w        = scenario == 1 ? 0u : 1u;
        hybrid.bounce.y     = scenario == 2 ? 0u : 1u;
        hybrid.reflection.y = scenario == 2 ? 0u : 1u;
        hybrid.rejection.z  = scenario == 3 ? 0.0f : 1.0f;
        RHITexture* old[6]{};
        RHITexture* next[6]{};
        RHITexture* raw[3]{};
        for (uint32_t signal = 0; signal < 3; ++signal)
        {
            raw[signal] = Upload(graph, DataFormat::eR32G32B32A32SFloat, HeapVector<Vec4>(Pixels, current[signal]));
            old[signal] = Upload(graph, DataFormat::eR32G32B32A32SFloat, HeapVector<Vec4>(Pixels, previous[signal]));
        }
        HeapVector<Vec4>       positions(Pixels, Vec4(0, 0, 0, 3));
        HeapVector<glm::uvec2> surfaces(Pixels, glm::uvec2(0x80008000u, 1));
        HeapVector<Vec4>       normals(Pixels, Vec4(.5f, .5f, .5f, 7));
        surfaces[2].y  = 99;
        positions[3].z = 1;
        surfaces[4].x  = 0xffffffffu;
        normals[4]     = Vec4(1, 1, .5f, 7);
        old[3]         = Upload(graph, DataFormat::eR32G32B32A32SFloat, positions);
        old[4]         = Upload(graph, DataFormat::eR32G32UInt, surfaces);
        old[5]         = Upload(graph, DataFormat::eR32G32B32A32SFloat, normals);
        RDGComputePassDesc pass;
        pass.SetShaderProgramName("ProbePixelTemporalSP");
        Inputs(pass);
        for (uint32_t signal = 0; signal < 6; ++signal)
        {
            next[signal] = Texture(formats[signal]);
            pass.BindSampledTexture(NameID(fmt::format("previousProbe{}", names[signal])), sampler,
                                    old[signal]->GetDefaultView());
            pass.BindStorageImage(NameID(fmt::format("nextProbe{}", names[signal])), next[signal]->GetDefaultView(),
                                  RDGContentGuarantee::eFullWrite);
            if (signal < 3)
            {
                pass.BindSampledTexture(NameID(fmt::format("probeRaw{}", names[signal])), sampler,
                                        raw[signal]->GetDefaultView());
            }
        }
        Dispatch(graph, std::move(pass), Width, Height);
        RHIBuffer* readback[6]{};
        for (uint32_t signal = 0; signal < 6; ++signal)
        {
            if (signal != 4)
            {
                readback[signal] = ReadColor(graph, next[signal]);
            }
        }
        Execute(graph);
        ASSERT_FALSE(HasFatalFailure());
        for (uint32_t signal = 0; signal < 6; ++signal)
        {
            if (signal != 4)
            {
                SCOPED_TRACE(signal);
                const HeapVector<Vec4> result = ReadStaticBuffer<Vec4>(readback[signal]);
                for (uint32_t index : {0u, 1u, 2u, 3u, 4u, 76u, Pixels - 1})
                {
                    SCOPED_TRACE(index);
                    const bool valid       = index >= 2;
                    const bool reuse       = scenario != 1 && scenario != 3 && index == 76;
                    const bool reuseSignal = reuse && (signal == 0 || scenario != 2);
                    Vec4       expected(0);
                    if (signal < 3)
                    {
                        expected = valid ? current[signal] : signal == 2 ? Vec4(0, 0, 0, 1) : Vec4(0);
                        if (reuseSignal)
                        {
                            const float alpha = signal == 2 ? .125f : .25f;
                            expected          = previous[signal] * (1.0f - alpha) + current[signal] * alpha;
                            if (signal == 1)
                            {
                                expected.w = 4;
                            }
                        }
                    }
                    else if (signal == 3 && valid)
                    {
                        expected = Vec4(0, 0, index == Pixels - 1 ? 1.0f : 0.0f, reuse ? 4.0f : 1.0f);
                    }
                    else if (signal == 5 && valid)
                    {
                        expected = Vec4(.5f, .5f, .5f, reuseSignal ? 8.0f : 1.0f);
                    }
                    ExpectColor(result[index], expected);
                }
            }
        }
    }
}

INSTANTIATE_TEST_SUITE_P(SubmissionModes, ScreenProbeGatherIntegrationTest, testing::Values(0u, 1u, 2u, 3u));
