TEST_F(RenderCoreTest, GraphicsVariantsSwitchShaderBindingsAndPushConstantsWithinOneRenderPass)
{
    ShaderProgram* base     = CreateTestShaderProgram(device, "variant_base");
    ShaderProgram* masked   = CreateTestShaderProgram(device, "variant_masked");
    TestBuffer*    geometry = Buffer();
    RenderGraph    graph("graphics_variants");
    ASSERT_TRUE(graph.Begin());
    RDGGraphicsPassDesc desc;
    desc.SetShaderProgramName("variant_base");
    desc.pipelineStates.rasterizationState.cullMode = RHIPolygonCullMode::eDisabled;
    desc.SetRenderArea(0, 0, 8, 8);
    desc.BindVertexBuffer(geometry);
    desc.BindIndexBuffer(geometry);
    desc.BindValue("value", uint32_t(17));
    desc.AddColorOutput(DataFormat::eR8G8B8A8UNORM, 8, 8, "color");
    const uint32_t variant = desc.AddPipelineVariant("variant_masked", RHIPolygonCullMode::eBack);
    graph.AddGraphicsPass(std::move(desc)).RecordPassCommands([variant](RDGPassCmdEncoder& encoder) {
        for (uint32_t index : {0u, variant, 0u})
        {
            encoder.SetPipelineVariant(index);
            encoder.SetPushConstants(index);
            encoder.DrawIndexed(3, 1, 0, 0, 0);
        }
    });
    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
    EXPECT_EQ(rhi->graphics.renderingCount, 1u);
    ASSERT_EQ(rhi->graphics.indexedPipelines.size(), 3u);
    ASSERT_EQ(rhi->graphics.pushConstantPipelines.size(), 3u);
    ASSERT_EQ(rhi->graphics.values.size(), 3u);
    for (uint32_t index = 0; index < 3; ++index)
    {
        TestPipeline* pipeline = static_cast<TestPipeline*>(rhi->graphics.indexedPipelines[index]);
        EXPECT_EQ(pipeline->GetShader(), index == 1 ? masked->GetShader() : base->GetShader());
        EXPECT_EQ(pipeline->GetCullMode(), index == 1 ? RHIPolygonCullMode::eBack : RHIPolygonCullMode::eDisabled);
        EXPECT_EQ(rhi->graphics.pushConstantPipelines[index], pipeline);
        uint32_t value = 0;
        ASSERT_EQ(rhi->graphics.values[index].size(), sizeof(value));
        std::memcpy(&value, rhi->graphics.values[index].data(), sizeof(value));
        EXPECT_EQ(value, 17u);
    }
    device->DestroyBuffer(geometry);
}

TEST_F(RenderCoreTest, GraphicsVariantsRejectChangedResourceInterfacesAndUndeclaredSelections)
{
    CreateTestShaderProgram(device, "variant_base");
    ShaderProgram* variant = CreateTestShaderProgram(device, "variant_other");
    static_cast<TestShader*>(variant->GetShader())->SetDescriptorReadOnly("buffer");
    RenderGraph graph("invalid_variants");
    ASSERT_TRUE(graph.Begin());
    RDGGraphicsPassDesc desc;
    desc.SetShaderProgramName("variant_base");
    desc.SetRenderArea(0, 0, 8, 8);
    desc.AddColorOutput(DataFormat::eR8G8B8A8UNORM, 8, 8, "color");
    desc.AddPipelineVariant("variant_other", RHIPolygonCullMode::eBack);
    graph.AddGraphicsPass(desc);
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eBinding);
    EXPECT_EQ(rhi->pipelineCount, 0u);

    ASSERT_TRUE(graph.Begin());
    desc.pipelineVariants.clear();
    graph.AddGraphicsPass(desc).RecordPassCommands([](RDGPassCmdEncoder& encoder) { encoder.SetPipelineVariant(1); });
    ASSERT_TRUE(graph.End());
    const uint32_t submitted = rhi->finalizedLists;
    EXPECT_FALSE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eRange);
    EXPECT_EQ(rhi->finalizedLists, submitted);
}

TEST_F(RenderCoreTest, GraphicsVariantReplacementInvalidatesRecordedAndCompiledGraphs)
{
    CreateTestShaderProgram(device, "variant_base");
    for (bool compiled : {false, true})
    {
        SCOPED_TRACE(compiled);
        ShaderProgram* variant = CreateTestShaderProgram(device, "variant_reload");
        RenderGraph    graph("variant_reload");
        ASSERT_TRUE(graph.Begin());
        RDGGraphicsPassDesc desc;
        desc.SetShaderProgramName("variant_base");
        desc.SetRenderArea(0, 0, 8, 8);
        desc.AddColorOutput(DataFormat::eR8G8B8A8UNORM, 8, 8, "color");
        desc.AddPipelineVariant("variant_reload", RHIPolygonCullMode::eBack);
        graph.AddGraphicsPass(desc);
        ASSERT_TRUE(graph.End());
        if (compiled)
        {
            ASSERT_TRUE(device->ExecuteRenderGraph(graph));
        }
        const uint32_t submitted = rhi->finalizedLists;
        ASSERT_TRUE(variant->Init());
        EXPECT_FALSE(device->ExecuteRenderGraph(graph));
        EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eShader);
        EXPECT_EQ(rhi->finalizedLists, submitted);
    }
}

TEST_F(RenderCoreTest, GraphicsVariantCreationFailureReleasesPartiallyCompiledPipelines)
{
    CreateTestShaderProgram(device, "variant_base");
    CreateTestShaderProgram(device, "variant_other");
    RenderGraph graph("variant_failure");
    ASSERT_TRUE(graph.Begin());
    RDGGraphicsPassDesc desc;
    desc.SetShaderProgramName("variant_base");
    desc.SetRenderArea(0, 0, 8, 8);
    desc.AddColorOutput(DataFormat::eR8G8B8A8UNORM, 8, 8, "color");
    desc.AddPipelineVariant("variant_other", RHIPolygonCullMode::eBack);
    graph.AddGraphicsPass(desc);
    ASSERT_TRUE(graph.End());
    rhi->failPipelineCreationAt = 2;
    EXPECT_FALSE(device->ExecuteRenderGraph(graph));
    EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eAllocation);
    EXPECT_EQ(rhi->finalizedLists, 0u);
    ASSERT_EQ(rhi->createdPipelines.size(), 1u);
    const uint64_t id = rhi->createdPipelines[0]->GetStableId();
    ASSERT_TRUE(graph.Reset());
    device->InvalidateRDGPassCompilerForResize();
    device->NextFrame();
    device->NextFrame();
    EXPECT_TRUE(destroyed.contains(id));
}

TEST_F(RenderCoreTest, GBufferGroupsOpaqueMaskedAndMirroredDrawsWithoutChangingFrontFace)
{
    for (const std::array<const char*, 2>& files : {std::array<const char*, 2>{"GBufferSP", "SceneRenderer/offscreen.frag.spv"},
                                                    {"GBufferMaskedSP", "SceneRenderer/offscreen_masked.frag.spv"}})
    {
        RHIShaderCreateInfo info;
        info.stageFlags.SetFlags(RHIShaderStageFlagBits::eVertex, RHIShaderStageFlagBits::eFragment);
        info.spirvFileName[ToUnderlying(RHIShaderStage::eVertex)]   = "SceneRenderer/offscreen.vert.spv";
        info.spirvFileName[ToUnderlying(RHIShaderStage::eFragment)] = files[1];
        reflectedShaderInfos[files[0]]                              = info;
        CreateTestShaderProgram(device, files[0]);
    }
    TestViewport            viewport;
    HeapVector<RHIBuffer*>  buffers;
    HeapVector<RHITexture*> textures;
    AllocateRendererInputs(0, viewport, buffers, textures);
    sg::Scene                           source;
    HeapVector<UniquePtr<sg::Material>> materials;
    HeapVector<UniquePtr<sg::SubMesh>>  submeshes;
    HeapVector<UniquePtr<sg::Mesh>>     meshes;
    HeapVector<UniquePtr<sg::Node>>     nodes;
    // Reverse input order to exercise grouping. Blend and transmission stay out of the G-buffer.
    for (int32_t index = 7; index >= 0; --index)
    {
        materials.push_back(MakeUnique<sg::Material>("material"));
        sg::Material& material             = *materials.back();
        material.index                     = index;
        material.data.materialProperties.z = index % 3 == 0 ? 1.0f : 0.0f;
        material.data.surfaceProperties.y = index >= 3 && index < 6 ? float(sg::AlphaMode::Mask) : float(sg::AlphaMode::Opaque);
        if (index == 6)
        {
            material.data.surfaceProperties.y = float(sg::AlphaMode::Blend);
        }
        if (index == 7)
        {
            material.data.sheenColorTransmission.w = 1.0f;
        }
        submeshes.push_back(MakeUnique<sg::SubMesh>("triangle", index * 3, 3, 3));
        submeshes.back()->SetMaterial(index, &material);
        meshes.push_back(MakeUnique<sg::Mesh>("mesh"));
        meshes.back()->AddSubMesh(submeshes.back().Get());
        nodes.push_back(MakeUnique<sg::Node>(index, "node"));
        nodes.back()->AddComponent(meshes.back().Get());
        Mat4 transform(1.0f);
        // Include mirrored two-sided geometry as well as mirrored single-sided geometry.
        transform[0][0] = index % 3 != 2 ? -1.0f : 1.0f;
        nodes.back()->SetData(index, transform);
        source.AddRenderableNode(nodes.back().Get());
    }
    SceneData data;
    data.pScene = &source;
    RenderScene              scene(device, data);
    DeferredLightingRenderer lighting(device);
    lighting.Init();
    lighting.SetRenderScene(&scene);
    RenderGraph& graph = *device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph.Begin());
    lighting.BuildGBufferGraph(RenderView::FromViewport(viewport));
    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
    EXPECT_EQ(rhi->graphics.renderingCount, 1u);
    ASSERT_EQ(rhi->graphics.indexedDraws.size(), 6u);
    ASSERT_EQ(rhi->graphics.indexedPipelines.size(), 6u);
    const RHIPolygonCullMode cullModes[] = {RHIPolygonCullMode::eDisabled, RHIPolygonCullMode::eFront,
                                            RHIPolygonCullMode::eBack};
    for (uint32_t index = 0; index < 6; ++index)
    {
        TestPipeline* pipeline = static_cast<TestPipeline*>(rhi->graphics.indexedPipelines[index]);
        EXPECT_EQ(rhi->graphics.indexedDraws[index][0], index * 3);
        EXPECT_EQ(pipeline->GetCullMode(), cullModes[index % 3]);
        EXPECT_EQ(pipeline->GetFrontFace(), RHIPolygonFrontFace::eCounterClockWise);
        EXPECT_EQ(pipeline->GetShader()->GetCreateInfo().spirvFileName[ToUnderlying(RHIShaderStage::eFragment)],
                  index < 3 ? "SceneRenderer/offscreen.frag.spv" : "SceneRenderer/offscreen_masked.frag.spv");
    }
    lighting.Destroy();
    for (RHIBuffer* buffer : buffers)
    {
        device->DestroyBuffer(buffer);
    }
    for (RHITexture* texture : textures)
    {
        device->DestroyTexture(texture);
    }
    sceneInputs = {};
}
