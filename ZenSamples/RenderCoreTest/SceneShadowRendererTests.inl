TEST_F(RenderCoreTest, SceneShadowsRetryFailedAllocationsAndExcludeDisabledLights)
{
    sg::Scene source;
    source.GetAABB() = sg::AABB(Vec3(-1), Vec3(1));
    SceneData data{};
    data.pScene = &source;
    RenderScene scene(device, data);
    sceneInputs.uniforms.lightInfo.x = 1;
    sceneInputs.uniforms.lights[0]   = {Vec4(0, 0, 0, 4), Vec4(0, -1, 0, 1), Vec4(1),
                                        Vec4(0, 0, 1, 0)};
    for (uint32_t failure = 1; failure <= 2; ++failure)
    {
        SceneShadowRenderer shadows(device);
        rhi->failTextureCreationAt = rhi->textureCreations + failure;
        EXPECT_FALSE(shadows.Prepare(scene, true));
        rhi->failTextureCreationAt = 0;
        ASSERT_TRUE(shadows.Prepare(scene, true));
        RDGComputePassDesc lighting;
        shadows.BindLightingInputs(lighting);
        SceneShadowUniformData uniforms;
        ASSERT_EQ(lighting.valueByteStorage.size(), sizeof(uniforms));
        std::memcpy(&uniforms, lighting.valueByteStorage.data(), sizeof(uniforms));
        EXPECT_EQ(uniforms.lights[0].y, 6.0f);
        ASSERT_TRUE(shadows.Prepare(scene, false));
        shadows.BindLightingInputs(lighting);
        std::memcpy(&uniforms, lighting.valueByteStorage.data(), sizeof(uniforms));
        EXPECT_EQ(uniforms.lights[0].y, 0.0f);
        shadows.Destroy();
    }
}

TEST_F(RenderCoreTest, SceneShadowsCacheStaticFacesAndInvalidateAfterLightGeometryOrHandoffChanges)
{
    RHIShaderCreateInfo shader;
    shader.stageFlags.SetFlags(RHIShaderStageFlagBits::eVertex, RHIShaderStageFlagBits::eFragment);
    shader.spirvFileName[ToUnderlying(RHIShaderStage::eVertex)] =
        "ShadowMapping/scene_shadow.vert.spv";
    shader.spirvFileName[ToUnderlying(RHIShaderStage::eFragment)] =
        "ShadowMapping/scene_shadow.frag.spv";
    reflectedShaderInfos["SceneShadowSP"] = shader;
    CreateTestShaderProgram(device, "SceneShadowSP");
    CreateTestShaderProgram(device, "intent");
    sceneInputs.vertices  = Buffer(128);
    sceneInputs.indices   = Buffer();
    sceneInputs.nodes     = Buffer(128);
    sceneInputs.materials = Buffer(96);
    sceneInputs.textures.push_back(Texture());
    sg::Scene source;
    source.GetAABB() = sg::AABB(Vec3(-1), Vec3(1));
    SceneData data{};
    data.pScene = &source;
    RenderScene scene(device, data);
    sceneInputs.uniforms.lightInfo.x = 2;
    for (uint32_t index = 0; index < 2; ++index)
    {
        sceneInputs.uniforms.lights[index] = {Vec4(0, 0, 0, 4), Vec4(0, -1, 0, 1), Vec4(1),
                                              Vec4(0.9f, 0.8f, 1, 0)};
    }
    SceneShadowRenderer shadows(device);
    RenderGraph* graph  = device->GetCurrentFrameRDG();
    RDGMetrics& metrics = device->GetRDGMetrics();
    metrics.SetSink({});
    const uint32_t expectedFaces[] = {12, 0, 6, 0, 12, 12, 7, 6, 2, 6, 0, 0};
    for (uint32_t frame = 0; frame < std::size(expectedFaces); ++frame)
    {
        SCOPED_TRACE(frame);
        switch (frame)
        {
            case 2: sceneInputs.uniforms.lights[1].positionRange.x = 0.25f; break;
            case 3: sceneInputs.uniforms.lights[1].colorIntensity.x = 0.5f; break;
            case 6: sceneInputs.uniforms.lights[1].directionType.w = 2; break;
            case 7: sceneInputs.uniforms.lightInfo.x = 1; break;
            // GI caches visibility while lights are black; toggling intensity
            // neither loses their maps nor triggers redundant shadow rendering.
            case 10: sceneInputs.uniforms.lights[0].colorIntensity.w = 0; break;
            case 11: sceneInputs.uniforms.lights[0].colorIntensity.w = 2; break;
            default: break;
        }
        ASSERT_TRUE(shadows.Prepare(scene, frame != 8, frame >= 10));
        ASSERT_TRUE(graph->Begin());
        for (RHIBuffer* buffer :
             {sceneInputs.vertices, sceneInputs.indices, sceneInputs.nodes, sceneInputs.materials})
        {
            graph->GetResourceManager()->ImportHostWrittenBuffer(buffer);
        }
        if (frame == 0)
        {
            graph->AddTransferPass("InitializeMaterial")
                .ClearTexture(sceneInputs.textures[0], Color(1));
        }
        shadows.BuildRenderGraph(scene, frame >= 5 ? 2 : 1);
        graph->AddComputePass(IntentPass("keepalive"));
        metrics.RequestCapture();
        ASSERT_TRUE(graph->End()) << graph->GetResult().message;
        ASSERT_TRUE(device->ExecuteRenderGraph(*graph)) << graph->GetResult().message;
        shadows.OnRenderGraphExecuted(frame != 3);
        EXPECT_EQ(CountGIPasses(metrics.GetLastSnapshot(), "SceneShadowFace_"),
                  expectedFaces[frame]);
    }
    shadows.Destroy();
    for (RHIBuffer* buffer :
         {sceneInputs.vertices, sceneInputs.indices, sceneInputs.nodes, sceneInputs.materials})
    {
        device->DestroyBuffer(buffer);
    }
    device->DestroyTexture(sceneInputs.textures[0]);
}
