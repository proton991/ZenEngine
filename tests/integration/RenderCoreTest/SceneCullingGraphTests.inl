void RegisterSceneCullingGraphShaders(RenderDevice* device)
{
    const char* files[][3] = {
        {"GBufferSP", "SceneRenderer/offscreen.vert.spv", "SceneRenderer/offscreen.frag.spv"},
        {"GBufferIndirectSP", "SceneRenderer/offscreen_indirect.vert.spv", "SceneRenderer/offscreen_indirect.frag.spv"},
        {"GBufferMaskedIndirectSP", "SceneRenderer/offscreen_indirect.vert.spv",
         "SceneRenderer/offscreen_masked_indirect.frag.spv"}};
    for (const char* const(&entry)[3] : files)
    {
        RHIShaderCreateInfo info;
        info.stageFlags.SetFlags(RHIShaderStageFlagBits::eVertex, RHIShaderStageFlagBits::eFragment);
        info.spirvFileName[ToUnderlying(RHIShaderStage::eVertex)]   = entry[1];
        info.spirvFileName[ToUnderlying(RHIShaderStage::eFragment)] = entry[2];
        RegisterReflectedShader(entry[0], info);
        CreateTestShaderProgram(device, entry[0]);
    }
    RHIShaderCreateInfo compute;
    compute.stageFlags.SetFlag(RHIShaderStageFlagBits::eCompute);
    compute.spirvFileName[ToUnderlying(RHIShaderStage::eCompute)] = "SceneRenderer/frustum_cull.comp.spv";
    RegisterReflectedShader("SceneFrustumCullSP", compute);
    CreateTestShaderProgram(device, "SceneFrustumCullSP");
}

void EnableSceneCullingGraphCapabilities(RHIGPUInfo& info)
{
    info.supportDrawIndirectFirstInstance = true;
    info.supportMultiDrawIndirect         = true;
    info.maxDrawIndirectCount             = 2;
    info.maxVertexOutputComponents        = 128;
    info.maxFragmentInputComponents       = 128;
}

uint32_t CountSceneDrawUploads(const TestContext& context, size_t begin)
{
    uint32_t count = 0;
    for (size_t index = begin; index < context.bufferCopyDestinations.size(); ++index)
    {
        if (context.bufferCopyDestinations[index]->GetResourceTag() == "gbuffer_draw_data")
        {
            ++count;
        }
    }
    return count;
}

TEST_F(RenderCoreTest, CulledGBufferGraphBatchesShrinkAcrossFramesAndReuseUnchangedUploads)
{
    RegisterSceneCullingGraphShaders(device);
    EnableSceneCullingGraphCapabilities(rhi->info);
    rhi->info.maxComputeWorkGroupCount = {1, 1, 1};
    TestViewport            viewport;
    HeapVector<RHIBuffer*>  buffers;
    HeapVector<RHITexture*> textures;
    AllocateRendererInputs(0, viewport, buffers, textures);
    sg::Scene    source;
    sg::Material material("culling material");
    sg::SubMesh  primitive("culling primitive", 0, 3, 3);
    primitive.SetMaterial(0, &material);
    sg::Mesh mesh("culling mesh");
    mesh.AddSubMesh(&primitive);
    HeapVector<UniquePtr<sg::Node>> nodes;
    for (uint32_t index = 0; index < 137; ++index)
    {
        UniquePtr<sg::Node> node = MakeUnique<sg::Node>(index, "culling node");
        node->AddComponent(&mesh);
        node->SetData(index, Mat4(1));
        source.AddRenderableNode(node.Get());
        nodes.push_back(std::move(node));
        RenderScene::ShadowCaster caster;
        caster.bounds = sg::AABB(Vec3(-.25f, -.25f, .25f), Vec3(.25f, .25f, .75f));
        sceneInputs.shadowCasters.push_back(caster);
    }
    SceneData data{};
    data.pScene = &source;
    RenderScene              scene(device, data);
    DeferredLightingRenderer lighting(device);
    lighting.Init();
    lighting.SetRenderScene(&scene);
    const uint32_t counts[] = {137, 137, 65, 65, 1, 0};
    for (uint32_t frame = 0; frame < 6; ++frame)
    {
        SCOPED_TRACE(frame);
        rhi->completed = rhi->submitted;
        device->NextFrame();
        source.GetRenderableNodes().resize(counts[frame]);
        rhi->info.supportMultiDrawIndirect      = frame < 3;
        sceneInputs.camera.projViewMatrix[3][0] = .01f * frame;
        const size_t graphicsCopies             = rhi->graphics.bufferCopyDestinations.size();
        const size_t transferCopies             = rhi->transfer.bufferCopyDestinations.size();
        rhi->graphics.indirectDrawCalls.clear();
        rhi->graphics.indexedDraws.clear();
        rhi->graphics.dispatches.clear();
        rhi->graphics.pushConstants.clear();
        RenderGraph& graph = *device->GetCurrentFrameRDG();
        ASSERT_TRUE(graph.Begin());
        lighting.BuildGBufferGraph(RenderView::FromViewport(viewport));
        const SceneCullingResult result = lighting.GetGPUCullingStatus();
        EXPECT_EQ(result.status, frame == 5 ? SceneCullingStatus::eEmpty : SceneCullingStatus::eCulled);
        EXPECT_EQ(result.candidateCount, counts[frame]);
        const bool changed = frame == 0 || frame == 2 || frame == 4;
        EXPECT_EQ(result.uploadedBytes, changed ? counts[frame] * sizeof(GBufferDrawData) : 0u);
        ASSERT_TRUE(graph.End());
        ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
        const uint32_t limit   = frame < 3 ? 2u : 1u;
        const uint32_t batches = (counts[frame] + limit - 1) / limit;
        ASSERT_EQ(rhi->graphics.indirectDrawCalls.size(), batches);
        for (uint32_t batch = 0; batch < batches; ++batch)
        {
            const TestContext::IndirectDrawCall& draw = rhi->graphics.indirectDrawCalls[batch];
            EXPECT_EQ(draw.offset, batch * limit * sizeof(DrawIndexedIndirectCommand));
            EXPECT_EQ(draw.count, std::min(limit, counts[frame] - batch * limit));
            EXPECT_EQ(draw.stride, sizeof(DrawIndexedIndirectCommand));
            EXPECT_EQ(draw.material, material.index);
            EXPECT_EQ(draw.pushSize, sizeof(uint32_t));
            EXPECT_EQ(draw.pushOffset, 0u);
        }
        EXPECT_TRUE(rhi->graphics.indexedDraws.empty());
        const uint32_t dispatches = (counts[frame] + ZEN_SCENE_CULL_GROUP_SIZE - 1) / ZEN_SCENE_CULL_GROUP_SIZE;
        ASSERT_EQ(rhi->graphics.dispatches.size(), dispatches);
        // One 4-byte graphics material value for the single material batch,
        // regardless of how many count-limited indirect calls it requires.
        ASSERT_EQ(rhi->graphics.pushConstants.size(), dispatches + (counts[frame] != 0 ? 1u : 0u));
        for (uint32_t dispatch = 0; dispatch < dispatches; ++dispatch)
        {
            EXPECT_EQ(rhi->graphics.dispatches[dispatch].second, (TestDispatchExtent{1, 1, 1}));
            ASSERT_EQ(rhi->graphics.pushConstants[dispatch].size(), sizeof(glm::uvec4));
            glm::uvec4 range;
            std::memcpy(&range, rhi->graphics.pushConstants[dispatch].data(), sizeof(range));
            EXPECT_EQ(range.x, dispatch * ZEN_SCENE_CULL_GROUP_SIZE);
            EXPECT_EQ(range.y,
                      std::min(uint32_t(ZEN_SCENE_CULL_GROUP_SIZE), counts[frame] - dispatch * ZEN_SCENE_CULL_GROUP_SIZE));
        }
        EXPECT_EQ(CountSceneDrawUploads(rhi->graphics, graphicsCopies) + CountSceneDrawUploads(rhi->transfer, transferCopies),
                  changed ? 1u : 0u);
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

TEST_F(RenderCoreTest, CulledGBufferGraphReportsCapabilityLimitAndDisabledFallbacks)
{
    RegisterSceneCullingGraphShaders(device);
    EnableSceneCullingGraphCapabilities(rhi->info);
    TestViewport            viewport;
    HeapVector<RHIBuffer*>  buffers;
    HeapVector<RHITexture*> textures;
    AllocateRendererInputs(0, viewport, buffers, textures);
    sg::Scene    source;
    sg::Material material("fallback material");
    sg::SubMesh  primitive("fallback primitive", 0, 3, 3);
    primitive.SetMaterial(0, &material);
    sg::Mesh mesh("fallback mesh");
    mesh.AddSubMesh(&primitive);
    sg::Node node(0, "fallback node");
    node.AddComponent(&mesh);
    source.AddRenderableNode(&node);
    SceneData data{};
    data.pScene = &source;
    RenderScene              scene(device, data);
    DeferredLightingRenderer lighting(device);
    lighting.Init();
    lighting.SetRenderScene(&scene);
    const RHIGPUInfo         supported  = rhi->info;
    const SceneCullingStatus expected[] = {SceneCullingStatus::eUnsupportedFirstInstance,
                                           SceneCullingStatus::eBufferLimit,
                                           SceneCullingStatus::eDispatchLimit,
                                           SceneCullingStatus::eDisabled,
                                           SceneCullingStatus::eUploadFailed,
                                           SceneCullingStatus::eCulled,
                                           SceneCullingStatus::eCulled};
    // Device checks run first, so unsupported devices never group or count candidates.
    const uint32_t expectedCandidates[] = {0, 2, 1, 0, 1, 1, 1};
    for (uint32_t scenario = 0; scenario < 7; ++scenario)
    {
        SCOPED_TRACE(scenario);
        rhi->info = supported;
        source.GetRenderableNodes().resize(1);
        if (scenario == 0)
        {
            rhi->info.supportDrawIndirectFirstInstance = false;
        }
        else if (scenario == 1)
        {
            source.AddRenderableNode(&node);
            rhi->info.maxStorageBufferRange = 2 * sizeof(GBufferDrawData) - 1;
        }
        else if (scenario == 2)
        {
            rhi->info.maxComputeWorkGroupInvocations = ZEN_SCENE_CULL_GROUP_SIZE - 1;
        }
        else if (scenario == 4)
        {
            rhi->failBufferCreationAt = rhi->bufferCreations + 1;
        }
        else if (scenario == 6)
        {
            // Ordinary GBuffer shaders stay within the components every device guarantees.
            rhi->info.maxVertexOutputComponents  = 64;
            rhi->info.maxFragmentInputComponents = 64;
        }
        lighting.SetGPUCullingEnabled(scenario != 3);
        rhi->graphics.indirectDrawCalls.clear();
        rhi->graphics.indexedDraws.clear();
        RenderGraph& graph = *device->GetCurrentFrameRDG();
        ASSERT_TRUE(graph.Begin());
        lighting.BuildGBufferGraph(RenderView::FromViewport(viewport));
        EXPECT_EQ(lighting.GetGPUCullingStatus().status, expected[scenario]);
        EXPECT_EQ(lighting.GetGPUCullingStatus().candidateCount, expectedCandidates[scenario]);
        // The last scenario reuses the records uploaded by the one before it.
        EXPECT_EQ(lighting.GetGPUCullingStatus().uploadedBytes, scenario == 5 ? sizeof(GBufferDrawData) : 0u);
        ASSERT_TRUE(graph.End());
        ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
        const bool culled = expected[scenario] == SceneCullingStatus::eCulled;
        EXPECT_EQ(rhi->graphics.indirectDrawCalls.size(), culled ? 1u : 0u);
        EXPECT_EQ(rhi->graphics.indexedDraws.size(), culled ? 0u : scenario == 1 ? 2u : 1u);
        rhi->completed = rhi->submitted;
        device->NextFrame();
    }
    // Hybrid receivers add a varying past the guaranteed components.
    HybridGIRenderer      receiver(device);
    SceneCullingDrawCache hybridCache;
    RenderGraph           hybridGraph("CulledHybridVaryingLimit");
    RDGGraphicsPassDesc   hybridPass;
    for (uint32_t stage = 0; stage < 2; ++stage)
    {
        SCOPED_TRACE(stage);
        rhi->info                                                                                 = supported;
        (stage == 0 ? rhi->info.maxVertexOutputComponents : rhi->info.maxFragmentInputComponents) = 79;
        EXPECT_TRUE(SupportsGBufferCulling(rhi->info));
        EXPECT_FALSE(SupportsHybridGBufferCulling(rhi->info));
        EXPECT_EQ(AddCulledGBufferPass(*device, hybridCache, hybridGraph, hybridPass, scene, &receiver).status,
                  SceneCullingStatus::eVaryingLimit);
    }
    lighting.Destroy();
    rhi->info = supported;
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

RDGGraphicsPassDesc SceneCullMaterialGraphPass(const RenderScene& scene, RHISampler* sampler, RHITexture* depth)
{
    RDGGraphicsPassDesc  pass;
    RHIGfxPipelineStates states{};
    states.rasterizationState.cullMode = RHIPolygonCullMode::eDisabled;
    states.depthStencilState           = RHIGfxPipelineDepthStencilState::Create(true, true, RHIDepthCompareOperator::eLess);
    states.colorBlendState.AddAttachments(4);
    states.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewport);
    pass.SetPipelineStates(states);
    pass.SetRenderArea(0, 0, 8, 8);
    pass.AddColorOutput(DataFormat::eR16G16UNORM, 8, 8, "CullMaterialNormal");
    pass.AddColorOutput(DataFormat::eR8G8B8A8UNORM, 8, 8, "CullMaterialAlbedo");
    pass.AddColorOutput(DataFormat::eR8G8B8A8UNORM, 8, 8, "CullMaterialSurface");
    pass.AddColorOutput(DataFormat::eR16G16B16A16SFloat, 8, 8, "CullMaterialEmission");
    pass.AddDepthStencilOutput(depth);
    pass.BindStorageBuffer("NodeBuffer", scene.GetNodesDataSSBO());
    pass.BindStorageBuffer("MaterialBuffer", scene.GetMaterialsDataSSBO());
    pass.BindStorageBuffer("UVBuffer", scene.GetUVBuffer());
    BindSceneTextureArray(pass, sampler, scene.GetSceneTextureViews(), scene.GetSceneSamplers());
    pass.BindValue("uCameraData", scene.GetCameraUniformData(), sizeof(sg::CameraUniformData));
    pass.BindVertexBuffer(scene.GetVertexBuffer());
    pass.BindIndexBuffer(scene.GetIndexBuffer());
    return pass;
}

TEST_F(RenderCoreTest, CulledGBufferMaterialBatchesPreserveDrawRecordsAndOrderedRuns)
{
    RegisterSceneCullingGraphShaders(device);
    EnableSceneCullingGraphCapabilities(rhi->info);
    TestViewport            viewport;
    HeapVector<RHIBuffer*>  buffers;
    HeapVector<RHITexture*> textures;
    AllocateRendererInputs(0, viewport, buffers, textures);
    const uint32_t materialsByNode[] = {4, 1, 2, 5, 0, 3, 2, 5, 0, 3, 4, 1, 2, 4, 2, 1, 5, 1, 1, 1};
    // Independent expected order: shader/cull groups, then each material's first use;
    // equal-material entries keep their authored order.
    const uint32_t            sortedNodes[] = {4, 8, 10, 13, 0, 2, 6, 12, 14, 5, 9, 1, 7, 16, 3, 11, 15, 17, 18, 19};
    sg::Scene                 source;
    HeapVector<sg::Material*> materials;
    for (uint32_t index = 0; index < 6; ++index)
    {
        UniquePtr<sg::Material> material    = MakeUnique<sg::Material>("batch material");
        material->index                     = index;
        material->data.surfaceProperties.y  = index % 2 != 0 ? static_cast<float>(sg::AlphaMode::Mask) : 0;
        material->data.materialProperties.z = index % 3 == 0 ? 1.0f : 0.0f;
        materials.push_back(material.Get());
        source.AddComponent(std::move(material));
    }
    HeapVector<UniquePtr<sg::Node>> nodes;
    for (uint32_t index = 0; index < 20; ++index)
    {
        const uint32_t         material  = materialsByNode[index];
        UniquePtr<sg::SubMesh> primitive = MakeUnique<sg::SubMesh>("batch primitive", 3 * index, 3 * (1 + index % 2), 3);
        primitive->SetMaterial(material, materials[material]);
        UniquePtr<sg::Mesh> mesh = MakeUnique<sg::Mesh>("batch mesh");
        mesh->AddSubMesh(primitive.Get());
        UniquePtr<sg::Node> node = MakeUnique<sg::Node>(index, "batch node");
        node->AddComponent(mesh.Get());
        node->SetData(index, glm::scale(Mat4(1), Vec3(index < 17 && index % 3 == 1 ? -1.0f : 1.0f, 1, 1)));
        source.AddRenderableNode(node.Get());
        nodes.push_back(std::move(node));
        source.AddComponent(std::move(mesh));
        source.AddComponent(std::move(primitive));
        RenderScene::ShadowCaster caster;
        caster.bounds = sg::AABB(Vec3(float(index), -.25f, .25f), Vec3(float(index) + .5f, .25f, .75f));
        sceneInputs.shadowCasters.push_back(caster);
    }
    SceneData data{};
    data.pScene = &source;
    RenderScene           scene(device, data);
    SceneCullingDrawCache cache;
    RHISampler*           sampler                  = device->CreateSampler({});
    const uint32_t        expectedCalls[]          = {12, 19, 20};
    const uint32_t        expectedMaterialPushes[] = {9, 18, 9};
    for (uint32_t mode = 0; mode < 3; ++mode)
    {
        SCOPED_TRACE(mode);
        rhi->info.supportMultiDrawIndirect = mode != 2;
        rhi->graphics.indirectDrawCalls.clear();
        rhi->graphics.pushConstants.clear();
        RenderGraph graph("MaterialCullingBatches");
        ASSERT_TRUE(graph.Begin());
        RDGGraphicsPassDesc pass = SceneCullMaterialGraphPass(scene, sampler, viewport.depth);
        ASSERT_EQ(AddCulledGBufferPass(*device, cache, graph, pass, scene, nullptr, mode == 1).status,
                  SceneCullingStatus::eCulled);
        ASSERT_EQ(cache.contents.size(), 20u);
        for (uint32_t index = 0; index < cache.contents.size(); ++index)
        {
            const uint32_t         node   = mode == 1 ? index : sortedNodes[index];
            const GBufferDrawData& record = cache.contents[index];
            EXPECT_EQ(record.indices.x, node);
            EXPECT_EQ(record.geometry.x, 3 * (1 + node % 2));
            EXPECT_EQ(record.geometry.y, 3 * node);
            EXPECT_EQ(record.boundsMin, Vec4(sceneInputs.shadowCasters[node].bounds.GetMin(), 0));
            EXPECT_EQ(record.boundsMax, Vec4(sceneInputs.shadowCasters[node].bounds.GetMax(), 0));
        }
        ASSERT_TRUE(graph.End());
        ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
        ASSERT_EQ(rhi->graphics.indirectDrawCalls.size(), expectedCalls[mode]);
        uint32_t covered = 0;
        for (const TestContext::IndirectDrawCall& draw : rhi->graphics.indirectDrawCalls)
        {
            EXPECT_EQ(draw.offset, covered * sizeof(DrawIndexedIndirectCommand));
            EXPECT_LE(draw.count, mode == 2 ? 1u : 2u);
            EXPECT_GT(draw.count, 0u);
            EXPECT_EQ(draw.pushSize, sizeof(uint32_t));
            EXPECT_EQ(draw.pushOffset, 0u);
            ASSERT_LE(covered + draw.count, cache.contents.size());
            for (uint32_t index = covered; index < covered + draw.count; ++index)
            {
                const uint32_t node     = cache.contents[index].indices.x;
                const uint32_t material = materialsByNode[node];
                EXPECT_EQ(draw.material, material);
                const RHIPolygonCullMode cull = material % 3 == 0          ? RHIPolygonCullMode::eDisabled
                                              : node < 17 && node % 3 == 1 ? RHIPolygonCullMode::eFront
                                                                           : RHIPolygonCullMode::eBack;
                EXPECT_EQ(static_cast<TestPipeline*>(draw.pipeline)->GetCullMode(), cull);
                const std::string& fragment =
                    draw.pipeline->GetShader()->GetCreateInfo().spirvFileName[ToUnderlying(RHIShaderStage::eFragment)];
                EXPECT_EQ(fragment.find("masked") != std::string::npos, material % 2 != 0);
            }
            covered += draw.count;
        }
        EXPECT_EQ(covered, 20u);
        uint32_t materialPushes = 0;
        for (const std::vector<uint8_t>& bytes : rhi->graphics.pushConstants)
        {
            materialPushes += bytes.size() == sizeof(uint32_t) ? 1u : 0u;
        }
        EXPECT_EQ(materialPushes, expectedMaterialPushes[mode]);
        rhi->completed = rhi->submitted;
        device->NextFrame();
    }
    cache.Destroy(*device);
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
