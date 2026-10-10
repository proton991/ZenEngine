TEST_F(RenderCoreTest, HybridHistoriesPublishOnlySuccessfulFramesAndResizePerView)
{
    for (const std::array<const char*, 2>& shader :
         {std::array<const char*, 2>{"HybridTraceSP", "VoxelGI/hybrid_trace.comp.spv"},
          {"EnvironmentColumnsSP", "VoxelGI/environment_columns.comp.spv"},
          {"EnvironmentRowsSP", "VoxelGI/environment_rows.comp.spv"},
          {"HybridTemporalSP", "VoxelGI/hybrid_temporal.comp.spv"},
          {"HybridFilterSP", "VoxelGI/hybrid_filter.comp.spv"},
          {"HybridBiasSP", "VoxelGI/hybrid_bias.comp.spv"},
          {"VoxelFilterAlbedoSP", "VoxelGI/filter_albedo.comp.spv"},
          {"VoxelFilterRadianceSP", "VoxelGI/filter_radiance.comp.spv"},
          {"VoxelSkyIrradianceLegacySP", "VoxelGI/sky_irradiance_legacy.comp.spv"},
          {"VoxelInjectRadianceSP", "VoxelGI/inject_radiance.comp.spv"}})
    {
        RHIShaderCreateInfo info;
        info.stageFlags.SetFlag(RHIShaderStageFlagBits::eCompute);
        info.spirvFileName[ToUnderlying(RHIShaderStage::eCompute)] = shader[1];
        reflectedShaderInfos[shader[0]]                            = info;
        CreateTestShaderProgram(device, shader[0]);
    }
    RHIShaderCreateInfo receiverInfo;
    receiverInfo.stageFlags.SetFlags(RHIShaderStageFlagBits::eVertex, RHIShaderStageFlagBits::eFragment);
    receiverInfo.spirvFileName[ToUnderlying(RHIShaderStage::eVertex)]   = "SceneRenderer/receiver.vert.spv";
    receiverInfo.spirvFileName[ToUnderlying(RHIShaderStage::eFragment)] = "SceneRenderer/receiver.frag.spv";
    reflectedShaderInfos["HybridReceiverSP"]                            = receiverInfo;
    CreateTestShaderProgram(device, "HybridReceiverSP");
    TestViewport            viewport;
    HeapVector<RHIBuffer*>  ownedBuffers;
    HeapVector<RHITexture*> ownedTextures;
    AllocateRendererInputs(0, viewport, ownedBuffers, ownedTextures);
    sceneInputs.camera.projViewMatrix = Mat4(1);
    sceneInputs.camera.proj           = Mat4(1);
    sceneInputs.camera.view           = Mat4(1);
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
    DeferredLightingRenderer renderer(device);
    renderer.Init();
    renderer.SetRenderScene(&scene);
    RenderView        view  = RenderView::FromViewport(viewport);
    HybridGIRenderer* first = nullptr;
    for (uint32_t frame = 0; frame < 9; ++frame)
    {
        RenderGraph* graph = device->GetCurrentFrameRDG();
        ASSERT_TRUE(graph->Begin());
        if (frame == 0)
        {
            for (RHITexture* texture : ownedTextures)
            {
                if (texture != viewport.depth)
                {
                    graph->AddTransferPass("InitializeHybridTestInput").ClearTexture(texture, Color(0));
                }
            }
        }
        if (volumes.BeginVolumeUpdate(*graph))
        {
            for (RHITexture* texture :
                 {volumes.GetVoxelTextures().pAlbedo, volumes.GetVoxelTextures().pNormal, volumes.GetVoxelTextures().pEmissive})
            {
                graph->AddTransferPass("InitializeHybridVoxels").ClearTexture(texture, Color(0));
            }
        }
        view.historyId = frame == 3 ? 1 : 0;
        if (frame == 4)
        {
            view.width *= 2;
        }
        if (frame == 6)
        {
            VoxelGISettings changed = gi.GetSettings();
            changed.samples         = 2;
            ASSERT_TRUE(gi.SetSettings(changed));
        }
        if (frame == 7)
        {
            ASSERT_TRUE(scene.SetEnvironmentLighting(0.5f, 30.0f, true, false));
        }
        if (frame == 8)
        {
            renderer.SetRenderScene(&scene);
        }
        renderer.BuildGBufferGraph(view, true);
        HybridGIRenderer* hybrid = renderer.GetHybridGI();
        if (frame == 0)
        {
            first = hybrid;
        }
        if (frame == 3)
        {
            EXPECT_NE(first, hybrid);
        }
        else
        {
            EXPECT_EQ(first, hybrid);
        }
        gi.BuildRenderGraph();
        ASSERT_TRUE(hybrid->BuildRenderGraph(view, gi));
        EXPECT_EQ(hybrid->IsHistoryValid(), frame == 1 || frame == 5);
        if (frame == 6)
        {
            EXPECT_STREQ(hybrid->GetResetReason(), "settings");
        }
        if (frame == 7)
        {
            EXPECT_STREQ(hybrid->GetResetReason(), "environment");
        }
        const uint32_t before = hybrid->GetFrame();
        ASSERT_TRUE(graph->End()) << graph->GetResult().message;
        const bool accepted = frame != 1;
        if (accepted)
        {
            ASSERT_TRUE(device->ExecuteRenderGraph(*graph)) << graph->GetResult().message;
        }
        renderer.OnRenderGraphExecuted(accepted);
        gi.OnRenderGraphExecuted(accepted);
        volumes.OnRenderGraphExecuted(accepted);
        EXPECT_EQ(hybrid->GetFrame(), before + (accepted ? 1u : 0u));
        if (!accepted)
        {
            EXPECT_STREQ(hybrid->GetResetReason(), "failed_frame");
        }
        if (frame == 4)
        {
            EXPECT_STREQ(hybrid->GetResetReason(), "resize");
        }
    }
    renderer.Destroy();
    gi.Destroy();
    volumes.Destroy();
    for (RHIBuffer* buffer : ownedBuffers)
    {
        device->DestroyBuffer(buffer);
    }
    for (RHITexture* texture : ownedTextures)
    {
        device->DestroyTexture(texture);
    }
    sceneInputs = {};
}
