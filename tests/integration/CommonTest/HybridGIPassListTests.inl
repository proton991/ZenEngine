constexpr uint32_t kMaxPassListNodes = 512;

bool CompareNodeIds(const RDGNodeMetrics& first, const RDGNodeMetrics& second)
{
    return first.id < second.id;
}

// A recorded cache can still be bypassed by the trace shader. Check that the trace actually consumes its output.
bool TraceReadsResolvedWorldCache(RenderDevice& device)
{
    RDG_ID resolve, trace;
    for (const RDGNodeMetrics& node : device.GetRDGMetrics().GetLastSnapshot().nodes)
    {
        if (node.name == "WorldCacheResolve")
        {
            resolve = node.id;
        }
        if (node.name == "HybridTrace")
        {
            trace = node.id;
        }
    }
    bool readsCache = false;
    for (const RDGDependency& edge : device.GetCurrentFrameRDG()->GetDependencies())
    {
        readsCache |=
            edge.source == resolve && edge.destination == trace && edge.reason == RDGDependencyReason::eVersionProducer;
    }
    return readsCache;
}

// Live scene-graph passes of one rendered frame in recording order, without uploads and readbacks, as
// one comma-separated list, from the RDG metrics snapshot. `queues` names each pass's planned queue for
// diagnostics and documentation.
std::string CaptureLiveScenePasses(RenderDevice&     device,
                                   RendererServer&   server,
                                   const RenderView& view,
                                   bool&             submitted,
                                   std::string&      queues)
{
    std::string passes;
    submitted                           = server.DispatchRenderWorkloads(view) == RenderFrameResult::eSubmitted;
    const RDGMetricsSnapshot&  snapshot = device.GetRDGMetrics().GetLastSnapshot();
    HeapVector<RDGNodeMetrics> nodes;
    for (const RDGNodeMetrics& node : snapshot.nodes)
    {
        if (node.type != RDGNodeType::eTransferPass)
        {
            nodes.push_back(node);
        }
    }
    // Execution order depends on the submission mode; node identities follow recording order.
    std::sort(nodes.begin(), nodes.end(), CompareNodeIds);
    submitted = submitted && snapshot.sceneDispatch && snapshot.nodes.size() < kMaxPassListNodes;
    queues.clear();
    for (const RDGNodeMetrics& node : nodes)
    {
        passes += (passes.empty() ? "" : ", ") + node.name.ToString();
        queues += node.name.ToString() + "@" + RHIQueueName(node.plannedQueue) + " ";
    }
    return passes;
}

// The target frame flow of each GI tier (RenderPipelineHardeningPlan.md, "Final frame flow"): ReLAX
// denoising at half GI resolution; HW RT with RT pipelines and ReSTIR; Compute with the settled field.
// Steady-state live passes are pinned in recording order, so a new, removed or reordered pass changes
// this list and that table deliberately.
TEST_P(RayQueryIntegrationTest, HybridGIFramePassListsArePinned)
{
    if (SupportsHybridRayTracingPipeline(device->GetGPUInfo()) && device->GetGPUInfo().rayQuery.IsUsable())
    {
        ShaderProgramManager::GetInstance().BuildShaderPrograms(device);
        device->InitializeRendererServer();
        RendererServer&          server = *device->GetRendererServer();
        ForwardBlendSceneFixture fixture(*device);
        ASSERT_TRUE(fixture.Init(1, false, 2, true));
        ASSERT_TRUE(fixture.scene->SetVoxelBounds(sg::AABB(Vec3(-2, -2, -5), Vec3(2, 2, 0))));
        ASSERT_TRUE(fixture.scene->SetEnvironmentLighting(.5f, 0, true, false));
        // Exercise glossy tracing and filtering on real receivers in both native backends;
        // an all-rough fixture only dispatches threads that skip that work.
        sg::MaterialData glossy = fixture.scene->GetMaterialsData()[1];
        glossy.roughnessFactor  = .18f;
        ASSERT_TRUE(fixture.scene->UpdateMaterial(1, glossy));
        SceneLight light;
        light.type         = SceneLightType::eDirectional;
        light.direction    = Vec3(0, 0, -1);
        light.intensity    = 1;
        light.castsShadows = true;
        ASSERT_NE(fixture.scene->GetLights().Add(light), 0u);
        constexpr uint32_t side = 96;
        rc::TextureFormat  format;
        format.width = format.height = side;
        format.depth                 = 1;
        format.format                = DataFormat::eR8G8B8A8UNORM;
        RHITexture* color            = device->CreateTextureColorRT(format, {}, "PassListColor");
        format.format                = DataFormat::eD32SFloat;
        RHITexture* depth            = device->CreateTextureDepthStencilRT(format, {}, "PassListDepth");
        textures.push_back(color);
        textures.push_back(depth);
        ASSERT_NE(color, nullptr);
        ASSERT_NE(depth, nullptr);
        const RenderView  view{color, depth, side, side};
        RDGMetricsOptions metrics;
        metrics.logging.enabled     = true;
        metrics.logging.sampleEvery = 1;
        metrics.logging.minInterval = std::chrono::milliseconds(0);
        metrics.maxNodeDetails      = kMaxPassListNodes;
        device->GetRDGMetrics().Configure(metrics);

        // Steady state of a static scene: shadow maps, the field, voxelization and the radiance cache are
        // current, so only per-frame work is live. HW: ray-traced GI, environment guide, ReSTIR, ReLAX and
        // receiver visibility before deferred lighting. Compute: field trace and ReLAX; map shadows are cached.
        const std::string hardwarePasses =
            "SkyboxDraw, GBufferFrustumCull, OffScreen, LightOccluders, HybridDownsample, "
            "HybridGuidePrepare, HybridTrace, HybridGuideFinish, ReSTIRTemporal, ReSTIRSpatialDiffuse0, "
            "ReSTIRSpatialReflection0, ReSTIRSpatialDiffuse1, ReSTIRSpatialReflection1, GIDenoisePreblur, "
            "GIDenoiseTemporal, GIDenoiseHistoryFix, "
            "GIDenoiseAtrous0, GIDenoiseAtrous1, GIDenoiseAtrous2, HybridReconstruct, "
            "GIDenoiseStabilize, ReceiverVisibilityCoarse, ReceiverVisibility, ForwardBackground, SceneLighting, "
            "ForwardOpaqueDisplay, ForwardHiZ, ForwardHiZ, ForwardHiZ, ForwardHiZ, ForwardHiZ, "
            "ForwardHiZ, ForwardHiZ, ForwardCull, ForwardPrefix, ForwardCullScan, ForwardEmit, "
            "ForwardTranslucent, ForwardToneMap";
        const std::string computePasses = "SkyboxDraw, GBufferFrustumCull, OffScreen, HybridDownsample, HybridTrace, "
                                          "GIDenoisePreblur, GIDenoiseTemporal, GIDenoiseHistoryFix, GIDenoiseAtrous0, "
                                          "GIDenoiseAtrous1, GIDenoiseAtrous2, HybridReconstruct, GIDenoiseStabilize, "
                                          "ForwardBackground, SceneLighting, ForwardOpaqueDisplay, ForwardHiZ, ForwardHiZ, "
                                          "ForwardHiZ, ForwardHiZ, ForwardHiZ, ForwardHiZ, ForwardHiZ, ForwardCull, "
                                          "ForwardPrefix, ForwardCullScan, ForwardEmit, ForwardTranslucent, ForwardToneMap";

        std::string cachePasses         = hardwarePasses;
        cachePasses.insert(cachePasses.find("HybridGuidePrepare"), "GICacheUpdate, GICacheResolve, ");
        std::string probePasses = cachePasses;
        probePasses.insert(probePasses.find("HybridGuidePrepare"), "NativeProbeBenchmark, ");
        const std::string nativeGIPasses =
            "WorldCacheResolve, ScreenProbePlacement, ScreenProbeAdaptive, ScreenProbeProposal, "
            "ScreenProbeTrace, ScreenProbeTemporal, ScreenProbeSpatialHorizontal, ScreenProbeSpatial, ScreenProbePrefilter, "
            "ScreenProbeSpecularSpatial, GlossyTrace, GlossyTemporal, GlossyHistoryFix, GlossySpatial, ScreenProbeGather, GlossyUncovered, ";
        std::string fillHardware = "WorldCacheLighting, WorldCacheStart, ", fillField = fillHardware;
        for (uint32_t bounce = 0; bounce < 4; ++bounce)
        {
            fillHardware += "WorldCacheTraceHits, WorldCacheShade, ";
            fillField    += "WorldCacheShade, ";
        }
        std::string nativePasses = "SkyboxDraw, GBufferFrustumCull, OffScreen, LightOccluders, " + fillHardware + nativeGIPasses
                                 + hardwarePasses.substr(hardwarePasses.find("ReceiverVisibilityCoarse"));
        // The hardware probe trace allocates, samples, traces and shades its recorded samples in separate passes.
        nativePasses.replace(nativePasses.find("ScreenProbeTrace, "), std::string_view("ScreenProbeTrace, ").size(),
                             "ScreenProbeSampleHits, ScreenProbeTraceHits, ScreenProbeShadeHits, ScreenProbeAccumulate, ");
        const std::string nativeComputePasses = "SkyboxDraw, GBufferFrustumCull, OffScreen, " + fillField + nativeGIPasses
                                              + computePasses.substr(computePasses.find("ForwardBackground"));
        const bool        budgetAvailable     = device->GetGPUMemoryStats().budgetAvailable;
        for (uint32_t tier = 0; tier < 10; ++tier)
        {
            const bool hardware        = tier < 5 || tier == 9;
            const bool cache           = tier == 1;
            const bool probes          = tier == 2;
            const bool native          = tier == 3 || tier == 4 || tier == 6 || tier == 7;
            const bool pixelWorldCache = tier == 8 || tier == 9;
            const bool diffuseOnly     = tier == 3 || tier == 7;
            SCOPED_TRACE(hardware ? "HW RT tier" : "Compute tier");
            VoxelGIRuntimeSettings settings = server.GetVoxelGISettings();
            settings.resolution             = 64;
            settings.cone.rayProvider = hardware ? VoxelGISettings::RayProvider::Hardware : VoxelGISettings::RayProvider::Voxel;
            settings.cone.rayTracingPipeline     = hardware;
            settings.cone.radianceCache          = cache;
            settings.cone.nativeProbeBenchmark   = probes;
            settings.cone.worldRadianceCache     = native || pixelWorldCache;
            settings.cone.screenProbeGI          = native;
            settings.cone.specularOcclusion      = !diffuseOnly;
            settings.cone.restirGI               = hardware;
            settings.cone.pixelResolutionDivisor = 2;
            // Ray-traced shadows with reuse, as the optimal configuration: a coarse grid, then refinement.
            SetGIShadows(settings.cone, GIShadows::RayTracedReuse);
            server.RequestDeferredLightingRenderer()->SetDeterministicGuideCapture(false);
            ASSERT_TRUE(server.ApplyVoxelGISettings(settings));
            server.SetRenderOption(RenderOption::eVoxelGI);
            // Warm up until GI renders from a settled field, then require two identical steady frames.
            bool settled = false;
            for (uint32_t frame = 0; frame < 256 && !settled; ++frame)
            {
                ASSERT_EQ(server.DispatchRenderWorkloads(view), RenderFrameResult::eSubmitted);
                device->WaitForPreviousFrames();
                device->NextFrame();
                settled = frame >= 8 && server.GetRenderingStatus().effective == RenderAlgorithm::eVoxelGI
                       && !server.IsPreparingGI() && !server.IsRefiningGI();
            }
            ASSERT_TRUE(settled);
            EXPECT_EQ(server.RequestVoxelGI()->UsesHardwareQueries(), hardware);
            EXPECT_EQ(server.RequestVoxelGI()->UsesRayTracingPipeline(), hardware);
            EXPECT_EQ(server.RequestVoxelGI()->UsesFieldTraversal(), !hardware);
            bool              submitted = false;
            std::string       queues;
            const std::string first = CaptureLiveScenePasses(*device, server, view, submitted, queues);
            ASSERT_TRUE(submitted);
            if (pixelWorldCache)
            {
                EXPECT_EQ(TraceReadsResolvedWorldCache(*device), budgetAvailable);
                server.RequestDeferredLightingRenderer()->SetDeterministicGuideCapture(true);
            }
            device->WaitForPreviousFrames();
            device->NextFrame();
            const std::string second = CaptureLiveScenePasses(*device, server, view, submitted, queues);
            ASSERT_TRUE(submitted);
            if (pixelWorldCache)
            {
                EXPECT_EQ(TraceReadsResolvedWorldCache(*device), budgetAvailable);
            }
            device->WaitForPreviousFrames();
            device->NextFrame();
            RecordProperty(hardware ? "hardware_queues" : "compute_queues", queues);
            EXPECT_EQ(first, second);
            const HybridGIRenderer* hybrid = server.RequestDeferredLightingRenderer()->GetHybridGI();
            if (native && !budgetAvailable)
            {
                EXPECT_EQ(hybrid->GetWorldCacheCapacity(), 0u);
                EXPECT_EQ(hybrid->GetScreenProbeCount(), 0u);
                EXPECT_EQ(hybrid->GetGIExtent(), glm::uvec2(side));
                EXPECT_NE(second.find("HybridTrace"), std::string::npos);
                EXPECT_EQ(second.find("ScreenProbeGather"), std::string::npos);
                continue;
            }
            std::string expected = native                    ? (hardware ? nativePasses : nativeComputePasses)
                                 : probes && budgetAvailable ? probePasses
                                 : cache && budgetAvailable  ? cachePasses
                                 : hardware                  ? hardwarePasses
                                                             : computePasses;
            if (pixelWorldCache)
            {
                if (budgetAvailable)
                {
                    expected.insert(expected.find(hardware ? "HybridGuidePrepare" : "HybridTrace"),
                                    (hardware ? fillHardware : fillField) + "WorldCacheResolve, ");
                }
                EXPECT_EQ(hybrid->GetWorldCacheCapacity() != 0u, budgetAvailable);
                EXPECT_EQ(hybrid->GetScreenProbeCount(), 0u);
            }
            if (diffuseOnly)
            {
                const size_t firstSpecular = expected.find("ScreenProbePrefilter");
                expected.erase(firstSpecular, expected.find("ScreenProbeGather") - firstSpecular);
                expected.erase(expected.find("GlossyUncovered, "), std::string_view("GlossyUncovered, ").size());
            }
            EXPECT_EQ(second, expected) << queues;
            if (native)
            {
                EXPECT_EQ(hybrid->GetGIExtent(), glm::uvec2(side));
                EXPECT_EQ(hybrid->GetScreenProbeCount(), diffuseOnly ? 288u : 180u);
                EXPECT_NE(hybrid->GetWorldCacheCapacity(), 0u);
            }
        }
    }
}
