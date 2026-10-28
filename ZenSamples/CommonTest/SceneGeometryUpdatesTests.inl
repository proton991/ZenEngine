glm::uvec4 ReadSceneTriangleRecord(RenderDevice* device, RHIBuffer* triangles, RHIBuffer* readback)
{
    RenderGraph graph("ReadSceneTriangleRecord");
    bool        valid = graph.Begin();
    if (valid)
    {
        RDGComputePassDesc pass;
        pass.SetShaderProgramName("TriangleRecordCheckSP");
        pass.BindStorageBuffer("Triangles", triangles);
        pass.BindStorageBuffer("Result", readback, RDGContentGuarantee::eFullWrite);
        graph.AddComputePass(std::move(pass)).RecordPassCommands([](RDGPassCmdEncoder& encoder) { encoder.Dispatch(1, 1, 1); });
        valid = graph.End() && device->ExecuteRenderGraph(graph);
        device->FlushRHIThread();
        device->WaitForIdle();
    }
    EXPECT_TRUE(valid) << graph.GetResult().message;
    glm::uvec4 result(UINT32_MAX);
    if (valid)
    {
        result = ReadStaticBuffer<glm::uvec4>(readback)[0];
    }
    return result;
}

void ExpectSceneTriangleRange(const RenderScene& scene, uint32_t instance, uint32_t first, uint32_t count, uint32_t mask)
{
    ASSERT_LT(instance, scene.GetInstanceTriangleRanges().size());
    const RenderScene::InstanceTriangleRange& range = scene.GetInstanceTriangleRanges()[instance];
    EXPECT_EQ(range.firstTriangle, first);
    EXPECT_EQ(range.triangleCount, count);
    EXPECT_EQ(range.committedMask, mask);
}

TEST_P(ConeVoxelGIIntegrationTest, SceneTriangleRecordsFollowMasksAndMaterialsAcrossPoseUpdates)
{
    ShaderProgramManager::GetInstance().StoreProgram(
        ZEN_NEW() ComputeFileSP(device, "TriangleRecordCheckSP", "VoxelGI/Calibration/triangle_record_check.comp.spv"));
    sg::Scene              source;
    sg::Node               node(0, "triangle");
    sg::Node               sharedNode(1, "shared triangle");
    sg::Mesh               mesh("triangle");
    UniquePtr<sg::SubMesh> primitive = MakeUnique<sg::SubMesh>("triangle", 0, 6, 3);
    for (uint32_t index = 0; index < 2; ++index)
    {
        UniquePtr<sg::Material> material = MakeUnique<sg::Material>("variant");
        material->index                  = index;
        if (index == 0)
        {
            primitive->SetMaterial(index, material.Get());
        }
        source.AddComponent(std::move(material));
    }
    primitive->assetMesh = primitive->assetPrimitive = 0;
    mesh.AddSubMesh(primitive.Get());
    mesh.SetAABB(Vec3(-1), Vec3(1));
    source.AddComponent(std::move(primitive));
    node.AddComponent(&mesh);
    node.SetData(0, Mat4(1));
    source.AddRenderableNode(&node);
    sharedNode.AddComponent(&mesh);
    sharedNode.SetData(1, glm::translate(Mat4(1), Vec3(4, 0, 0)));
    source.AddRenderableNode(&sharedNode);
    source.GetAABB() = sg::AABB(Vec3(-1), Vec3(1));
    source.GetAssetData().materialVariants.push_back("alternate");
    source.GetAssetData().variantPrimitives.push_back({0, 0, {1}});
    asset::Vertex vertices[4]{};
    vertices[0].pos          = Vec4(-1, -1, 0, 1);
    vertices[1].pos          = Vec4(1, -1, 0, 1);
    vertices[2].pos          = Vec4(0, 1, 0, 1);
    vertices[3].pos          = Vec4(1000, 1000, 1000, 1); // Unreferenced vertices must not expand bounds.
    const uint32_t indices[] = {0, 1, 2, 2, 1, 0};
    SceneData      data{};
    data.pScene      = &source;
    data.pVertices   = vertices;
    data.pIndices    = indices;
    data.numVertices = 4;
    data.numIndices  = 6;
    asset::PrepareScenePlacement(*data.pScene, asset::ScenePlacement::CenterAndNormalize);
    RenderScene scene(device, data);
    scene.LoadSceneMaterials();
    scene.PrepareBuffers();
    RHIBuffer* readback = Buffer(sizeof(glm::uvec4), RHIBufferAllocateType::eCPURead);
    RHIBuffer* original = scene.GetVoxelTriangleBuffer();
    EXPECT_EQ(scene.GetVoxelTriangleCount(), 4u);
    ASSERT_EQ(scene.GetInstanceTriangleRanges().size(), 2u);
    ExpectSceneTriangleRange(scene, 0, 0, 2, GI_STATIC);
    ExpectSceneTriangleRange(scene, 1, 2, 2, GI_STATIC);
    EXPECT_EQ(ReadSceneTriangleRecord(device, original, readback), glm::uvec4(0, 0, 0, GI_STATIC));

    HeapVector<RenderScene::ShadowCaster> casters = scene.GetShadowCasters();
    ASSERT_EQ(casters.size(), 2u);

    EXPECT_TRUE(scene.SetInstanceTransform(0, glm::translate(Mat4(1), Vec3(0.5f, 0, 0))));
    EXPECT_EQ(scene.GetShadowCasters()[0].revision, casters[0].revision);
    EXPECT_EQ(scene.GetShadowCasters()[0].bounds, casters[0].bounds);
    EXPECT_TRUE(scene.CommitGeometryUpdates());
    EXPECT_GT(scene.GetShadowCasters()[0].revision, casters[0].revision);
    EXPECT_NE(scene.GetShadowCasters()[0].bounds, casters[0].bounds);
    EXPECT_EQ(scene.GetShadowCasters()[1].revision, casters[1].revision);
    EXPECT_EQ(scene.GetShadowCasters()[1].bounds, casters[1].bounds);
    EXPECT_EQ(scene.GetVoxelTriangleBuffer(), original);
    ExpectSceneTriangleRange(scene, 0, 0, 2, GI_STATIC);
    ExpectSceneTriangleRange(scene, 1, 2, 2, GI_STATIC);
    EXPECT_EQ(scene.GetAABB(), sg::AABB(Vec3(-0.5f, -1, -0.5f), Vec3(2.5f, 1, 0.5f)));
    casters           = scene.GetShadowCasters();
    vertices[2].pos.y = 0.75f;
    EXPECT_TRUE(scene.UpdateVertices(0, VectorView<const asset::Vertex>(vertices, 3)));
    EXPECT_TRUE(scene.CommitGeometryUpdates());
    EXPECT_EQ(scene.GetVoxelTriangleBuffer(), original);
    EXPECT_EQ(scene.GetAABB(), sg::AABB(Vec3(-0.5f, -1, -0.5f), Vec3(2.5f, 0.75f, 0.5f)));
    EXPECT_EQ(ReadSceneTriangleRecord(device, scene.GetVoxelTriangleBuffer(), readback), glm::uvec4(0, 0, 0, GI_STATIC));
    EXPECT_GT(scene.GetShadowCasters()[0].revision, casters[0].revision);
    EXPECT_GT(scene.GetShadowCasters()[1].revision, casters[1].revision);

    // Shared UV edits change coverage without changing either world-space bound.
    casters         = scene.GetShadowCasters();
    vertices[0].uv0 = Vec2(0.2f, 0.4f);
    EXPECT_TRUE(scene.UpdateVertices(0, MakeVecView(vertices, 1)));
    EXPECT_TRUE(scene.CommitGeometryUpdates());
    for (uint32_t instance = 0; instance < 2; ++instance)
    {
        EXPECT_GT(scene.GetShadowCasters()[instance].revision, casters[instance].revision);
        EXPECT_EQ(scene.GetShadowCasters()[instance].bounds, casters[instance].bounds);
    }

    casters = scene.GetShadowCasters();
    EXPECT_TRUE(scene.SetInstanceTransform(0, scene.GetInstanceTransform(0)));
    EXPECT_TRUE(scene.UpdateVertices(0, MakeVecView(vertices, 1)));
    EXPECT_TRUE(scene.CommitGeometryUpdates());
    EXPECT_EQ(scene.GetShadowCasters()[0].revision, casters[0].revision);
    EXPECT_EQ(scene.GetShadowCasters()[1].revision, casters[1].revision);

    // A coverage change updates voxel geometry but keeps the same material assignment.
    sg::MaterialData material  = scene.GetMaterialsData()[0];
    material.baseColorFactor.a = 0.5f;
    EXPECT_TRUE(scene.UpdateMaterial(0, material));
    EXPECT_TRUE(scene.CommitGeometryUpdates());
    EXPECT_EQ(scene.GetVoxelTriangleBuffer(), original);
    EXPECT_GT(scene.GetShadowCasters()[0].revision, casters[0].revision);
    EXPECT_GT(scene.GetShadowCasters()[1].revision, casters[1].revision);

    casters = scene.GetShadowCasters();
    EXPECT_TRUE(scene.SetInstanceClass(0, GI_DYNAMIC));
    ExpectSceneTriangleRange(scene, 0, 0, 2, GI_STATIC);
    EXPECT_TRUE(scene.CommitGeometryUpdates());
    ExpectSceneTriangleRange(scene, 0, 0, 2, GI_DYNAMIC);
    ExpectSceneTriangleRange(scene, 1, 2, 2, GI_STATIC);
    EXPECT_GT(scene.GetShadowCasters()[0].revision, casters[0].revision);
    EXPECT_EQ(scene.GetShadowCasters()[1].revision, casters[1].revision);
    EXPECT_EQ(ReadSceneTriangleRecord(device, scene.GetVoxelTriangleBuffer(), readback), glm::uvec4(0, 0, 0, GI_DYNAMIC));
    casters = scene.GetShadowCasters();
    EXPECT_TRUE(scene.SetInstanceEnabled(0, false));
    ExpectSceneTriangleRange(scene, 0, 0, 2, GI_DYNAMIC);
    EXPECT_TRUE(scene.CommitGeometryUpdates());
    ExpectSceneTriangleRange(scene, 0, 0, 2, 0);
    EXPECT_GT(scene.GetShadowCasters()[0].revision, casters[0].revision);
    EXPECT_EQ(scene.GetShadowCasters()[0].bounds, sg::AABB());
    EXPECT_EQ(scene.GetShadowCasters()[1].revision, casters[1].revision);
    EXPECT_EQ(ReadSceneTriangleRecord(device, scene.GetVoxelTriangleBuffer(), readback), glm::uvec4(0, 0, 0, 0));
    EXPECT_EQ(scene.GetAABB(), sg::AABB(Vec3(-0.5f), Vec3(2.5f, 0.5f, 0.5f)));
    EXPECT_TRUE(scene.SetMaterialVariant(0));
    EXPECT_TRUE(scene.SetInstanceEnabled(0, true));
    ExpectSceneTriangleRange(scene, 0, 0, 2, 0);
    EXPECT_TRUE(scene.CommitGeometryUpdates());
    ExpectSceneTriangleRange(scene, 0, 0, 2, GI_DYNAMIC);
    EXPECT_NE(scene.GetShadowCasters()[0].bounds, sg::AABB());
    EXPECT_EQ(ReadSceneTriangleRecord(device, scene.GetVoxelTriangleBuffer(), readback), glm::uvec4(0, 0, 1, GI_DYNAMIC));
    EXPECT_TRUE(scene.SetMaterialVariant(-1));
    EXPECT_TRUE(scene.CommitGeometryUpdates());
    EXPECT_EQ(ReadSceneTriangleRecord(device, scene.GetVoxelTriangleBuffer(), readback), glm::uvec4(0, 0, 0, GI_DYNAMIC));

    // Authored visibility is observed by the animation path, independently of the explicit mask.
    source.GetAssetData().animations.emplace_back();
    EXPECT_TRUE(scene.SetAnimation(0));
    for (bool visible : {false, true})
    {
        casters      = scene.GetShadowCasters();
        node.visible = visible;
        EXPECT_TRUE(scene.AdvanceAnimation(1.0f / 60.0f));
        EXPECT_TRUE(scene.CommitGeometryUpdates());
        EXPECT_EQ(ReadSceneTriangleRecord(device, scene.GetVoxelTriangleBuffer(), readback),
                  glm::uvec4(0, 0, 0, visible ? GI_DYNAMIC : 0));
        ExpectSceneTriangleRange(scene, 0, 0, 2, visible ? GI_DYNAMIC : 0);
        EXPECT_GT(scene.GetShadowCasters()[0].revision, casters[0].revision);
        EXPECT_EQ(scene.GetShadowCasters()[0].bounds == sg::AABB(), !visible);
    }

    // Exercise the real scene update path across ring wrap, including a sparse
    // UV/material edit that must reach each slot when that slot is next used.
    RHIBuffer* snapshots[5]{};
    RHIBuffer* sceneBuffers[] = {scene.GetVertexBuffer(), scene.GetNodesDataSSBO(), scene.GetUVBuffer(),
                                 scene.GetMaterialsDataSSBO(), scene.GetVertexBuffer()};
    for (uint32_t i = 0; i < 5; ++i)
    {
        snapshots[i] = Buffer(static_cast<uint32_t>(sceneBuffers[i]->GetRequiredSize()), RHIBufferAllocateType::eCPURead);
    }
    RHIBuffer*     vertexSlots[RenderFrameState::kMaxFramesInFlight]{};
    uint64_t       vertexGenerations[RenderFrameState::kMaxFramesInFlight]{};
    const uint32_t slots = GRenderFrameState.GetNumFramesInFlight();
    for (uint32_t frame = 0; frame < 4 * slots; ++frame)
    {
        RHIResourcePtr<RHIBuffer>       previous(scene.GetVertexBuffer());
        const uint64_t                  previousGeneration = scene.GetVertexGeneration();
        const HeapVector<asset::Vertex> previousVertices   = scene.GetVertices();
        device->NextFrame();
        const uint32_t slot = ToIndex(GRenderFrameState.GetFrameSlot());
        vertices[0].pos.x   = -0.9f + 0.01f * frame;
        if (frame == 1)
        {
            vertices[0].uv0 = Vec2(0.25f, 0.75f);
        }
        ASSERT_TRUE(scene.UpdateVertices(0, MakeVecView(vertices, 1)));
        ASSERT_TRUE(scene.SetInstanceTransform(0, glm::translate(Mat4(1), Vec3(0.02f * frame, 0, 0))));
        if (frame == 2)
        {
            material.baseColorFactor.r = 0.7f;
            ASSERT_TRUE(scene.UpdateMaterial(0, material));
        }
        ASSERT_TRUE(scene.CommitGeometryUpdates());
        EXPECT_TRUE(scene.HasVertexGeneration(previous.Get(), previousGeneration));
        if (frame >= slots)
        {
            EXPECT_EQ(scene.GetVertexBuffer(), vertexSlots[slot]);
            EXPECT_FALSE(scene.HasVertexGeneration(vertexSlots[slot], vertexGenerations[slot]));
        }
        vertexSlots[slot]       = scene.GetVertexBuffer();
        vertexGenerations[slot] = scene.GetVertexGeneration();
        RHIBuffer*  inputs[]    = {scene.GetVertexBuffer(), scene.GetNodesDataSSBO(), scene.GetUVBuffer(),
                                   scene.GetMaterialsDataSSBO(), previous.Get()};
        RenderGraph graph("ReadPersistentSceneBuffers");
        ASSERT_TRUE(graph.Begin());
        for (uint32_t i = 0; i < 5; ++i)
        {
            RDGComputePassDesc pass;
            pass.SetShaderProgramName("CaptureVoxelBufferSP");
            pass.BindStorageBuffer("CaptureSource", inputs[i]);
            pass.BindStorageBuffer("CaptureCopy", snapshots[i], RDGContentGuarantee::eFullWrite);
            graph.AddComputePass(std::move(pass)).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
                encoder.Dispatch(1, 1, 1);
            });
        }
        ASSERT_TRUE(graph.End()) << graph.GetResult().message;
        ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
        device->WaitForPreviousFrames();
        const HeapVector<uint8_t> gpuVertices  = ReadStaticBuffer<uint8_t>(snapshots[0]);
        const HeapVector<uint8_t> gpuNodes     = ReadStaticBuffer<uint8_t>(snapshots[1]);
        const HeapVector<Vec4>    gpuUV        = ReadStaticBuffer<Vec4>(snapshots[2]);
        const HeapVector<uint8_t> gpuMaterials = ReadStaticBuffer<uint8_t>(snapshots[3]);
        const HeapVector<uint8_t> gpuPrevious  = ReadStaticBuffer<uint8_t>(snapshots[4]);
        EXPECT_EQ(
            std::memcmp(gpuVertices.data(), scene.GetVertices().data(), scene.GetVertices().size() * sizeof(asset::Vertex)), 0);
        EXPECT_EQ(std::memcmp(gpuPrevious.data(), previousVertices.data(), previousVertices.size() * sizeof(asset::Vertex)), 0);
        EXPECT_EQ(std::memcmp(gpuNodes.data(), &node.GetData(), sizeof(sg::NodeData)), 0);
        EXPECT_EQ(std::memcmp(gpuNodes.data() + sizeof(sg::NodeData), &sharedNode.GetData(), sizeof(sg::NodeData)), 0);
        EXPECT_EQ(Vec2(gpuUV[1]), vertices[0].uv0);
        EXPECT_EQ(std::memcmp(gpuMaterials.data(), scene.GetMaterialsData().data(),
                              scene.GetMaterialsData().size() * sizeof(sg::MaterialData)),
                  0);
    }

    // A rejected generation cannot publish staged masks/ranges or replace committed bounds.
    casters                                                                = scene.GetShadowCasters();
    const HeapVector<RenderScene::InstanceTriangleRange> ranges            = scene.GetInstanceTriangleRanges();
    RHIBuffer*                                           committedRecords  = scene.GetVoxelTriangleBuffer();
    const uint64_t                                       committedRevision = scene.GetGeometryRevision();
    EXPECT_TRUE(scene.SetInstanceClass(0, GI_STATIC));
    EXPECT_TRUE(scene.SetInstanceEnabled(1, false));
    asset::Vertex overflow = scene.GetVertices()[0];
    overflow.pos.x         = std::numeric_limits<float>::max();
    EXPECT_TRUE(scene.UpdateVertices(0, MakeVecView(&overflow, 1)));
    EXPECT_FALSE(scene.CommitGeometryUpdates());
    EXPECT_EQ(scene.GetVoxelTriangleBuffer(), committedRecords);
    EXPECT_EQ(scene.GetVoxelTriangleCount(), 4u);
    EXPECT_EQ(scene.GetGeometryRevision(), committedRevision);
    for (uint32_t instance = 0; instance < 2; ++instance)
    {
        ExpectSceneTriangleRange(scene, instance, ranges[instance].firstTriangle, ranges[instance].triangleCount,
                                 ranges[instance].committedMask);
        EXPECT_EQ(scene.GetShadowCasters()[instance].revision, casters[instance].revision);
        EXPECT_EQ(scene.GetShadowCasters()[instance].bounds, casters[instance].bounds);
    }
    scene.Destroy();
    EXPECT_TRUE(scene.GetInstanceTriangleRanges().empty());
}

TEST_P(ConeVoxelGIIntegrationTest, SceneTriangleRangesPreserveSubmeshOrderAndEmptyInstances)
{
    sg::Scene               source;
    sg::Node                triangleNode(0, "triangle primitives"), pointNode(1, "point"), emptyNode(2, "empty");
    sg::Mesh                triangleMesh("triangle primitives"), pointMesh("point"), emptyMesh("empty");
    UniquePtr<sg::Material> material    = MakeUnique<sg::Material>("transparent coverage");
    material->index                     = 0;
    material->data.surfaceProperties.y  = static_cast<float>(sg::AlphaMode::Blend);
    material->data.baseColorFactor.a    = 0.0f;
    const uint32_t         starts[]     = {0, 3, 5, 8};
    const uint32_t         counts[]     = {3, 2, 3, 1};
    const sg::MeshTopology topologies[] = {sg::MeshTopology::Triangles, sg::MeshTopology::Lines, sg::MeshTopology::Triangles,
                                           sg::MeshTopology::Points};
    for (uint32_t index = 0; index < 4; ++index)
    {
        UniquePtr<sg::SubMesh> primitive = MakeUnique<sg::SubMesh>("primitive", starts[index], counts[index], 3);
        primitive->topology              = topologies[index];
        primitive->SetMaterial(0, material.Get());
        (index < 3 ? triangleMesh : pointMesh).AddSubMesh(primitive.Get());
        source.AddComponent(std::move(primitive));
    }
    source.AddComponent(std::move(material));
    sg::Node* nodes[]  = {&triangleNode, &pointNode, &emptyNode};
    sg::Mesh* meshes[] = {&triangleMesh, &pointMesh, &emptyMesh};
    for (uint32_t instance = 0; instance < 3; ++instance)
    {
        meshes[instance]->SetAABB(Vec3(-1), Vec3(1));
        nodes[instance]->AddComponent(meshes[instance]);
        nodes[instance]->SetData(instance, Mat4(1));
        source.AddRenderableNode(nodes[instance]);
    }
    source.GetAABB() = sg::AABB(Vec3(-1), Vec3(1));
    asset::Vertex vertices[3]{};
    vertices[0].pos          = Vec4(-1, -1, 0, 1);
    vertices[1].pos          = Vec4(1, -1, 0, 1);
    vertices[2].pos          = Vec4(0, 1, 0, 1);
    const uint32_t indices[] = {0, 1, 2, 0, 1, 2, 1, 0, 2};
    SceneData      data{};
    data.pScene      = &source;
    data.pVertices   = vertices;
    data.pIndices    = indices;
    data.numVertices = 3;
    data.numIndices  = 9;
    RenderScene scene(device, data);
    EXPECT_TRUE(scene.GetInstanceTriangleRanges().empty());
    scene.LoadSceneMaterials();
    scene.PrepareBuffers();
    ASSERT_EQ(scene.GetInstanceTriangleRanges().size(), 3u);
    EXPECT_EQ(scene.GetVoxelTriangleCount(), 2u);
    ExpectSceneTriangleRange(scene, 0, 0, 2, GI_STATIC);
    ExpectSceneTriangleRange(scene, 1, 2, 0, GI_STATIC);
    ExpectSceneTriangleRange(scene, 2, 2, 0, GI_STATIC);
    const HeapVector<glm::uvec4> records = scene.BuildTriangleRecords();
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0], glm::uvec4(0, 0, 0, GI_STATIC));
    EXPECT_EQ(records[1], glm::uvec4(5, 0, 0, GI_STATIC));
    EXPECT_TRUE(scene.SetInstanceEnabled(1, false));
    ExpectSceneTriangleRange(scene, 1, 2, 0, GI_STATIC);
    EXPECT_TRUE(scene.CommitGeometryUpdates());
    ExpectSceneTriangleRange(scene, 1, 2, 0, 0);
    scene.Destroy();

    sg::Scene emptySource;
    sg::Node  onlyEmptyNode(0, "empty scene geometry");
    sg::Mesh  onlyEmptyMesh("empty scene geometry");
    onlyEmptyMesh.SetAABB(Vec3(-1), Vec3(1));
    onlyEmptyNode.AddComponent(&onlyEmptyMesh);
    onlyEmptyNode.SetData(0, Mat4(1));
    emptySource.AddRenderableNode(&onlyEmptyNode);
    SceneData emptyData{};
    emptyData.pScene = &emptySource;
    RenderScene emptyScene(device, emptyData);
    emptyScene.LoadSceneMaterials();
    emptyScene.PrepareBuffers();
    ASSERT_EQ(emptyScene.GetInstanceTriangleRanges().size(), 1u);
    ExpectSceneTriangleRange(emptyScene, 0, 0, 0, GI_STATIC);
    EXPECT_EQ(emptyScene.GetVoxelTriangleCount(), 0u);
    EXPECT_EQ(emptyScene.GetVoxelTriangleBuffer(), nullptr);
    EXPECT_TRUE(emptyScene.SetInstanceEnabled(0, false));
    EXPECT_TRUE(emptyScene.CommitGeometryUpdates());
    ExpectSceneTriangleRange(emptyScene, 0, 0, 0, 0);
    emptyScene.Destroy();
}

TEST_P(ConeVoxelGIIntegrationTest, BakedFbxMorphAndSkinPosesReachGpuGeometry)
{
    device->InitializeRendererServer();
    const std::string path =
        (std::filesystem::path(__FILE__).parent_path() / "Assets/FBX/combined_morph_skin.zenasset").string();
    asset::SceneLoadResult loaded = asset::LoadScene(path);
    ASSERT_TRUE(loaded.scene) << loaded.error;
    SceneData data{};
    data.pScene      = loaded.scene->scene.Get();
    data.pVertices   = loaded.scene->vertices.data();
    data.pIndices    = loaded.scene->indices.data();
    data.numVertices = static_cast<uint32_t>(loaded.scene->vertices.size());
    data.numIndices  = static_cast<uint32_t>(loaded.scene->indices.size());
    RenderScene scene(device, data);
    ASSERT_TRUE(scene.Init());
    ASSERT_TRUE(scene.SetAnimation(0, false));
    RHIBuffer* readback =
        Buffer(static_cast<uint32_t>(scene.GetVertexBuffer()->GetRequiredSize()), RHIBufferAllocateType::eCPURead);
    const HeapVector<asset::Vertex> initial = scene.GetVertices();
    for (float step : {0.0f, 0.23f, 0.27f})
    {
        const HeapVector<RenderScene::ShadowCaster> casters = scene.GetShadowCasters();
        ASSERT_TRUE(scene.AdvanceAnimation(step));
        ASSERT_TRUE(scene.CommitGeometryUpdates());
        RenderGraph graph("ReadBakedFbxPose");
        ASSERT_TRUE(graph.Begin());
        RDGComputePassDesc pass;
        pass.SetShaderProgramName("CaptureVoxelBufferSP");
        pass.BindStorageBuffer("CaptureSource", scene.GetVertexBuffer());
        pass.BindStorageBuffer("CaptureCopy", readback, RDGContentGuarantee::eFullWrite);
        graph.AddComputePass(std::move(pass)).RecordPassCommands([](RDGPassCmdEncoder& encoder) { encoder.Dispatch(1, 1, 1); });
        ASSERT_TRUE(graph.End()) << graph.GetResult().message;
        ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
        device->FlushRHIThread();
        device->WaitForIdle();
        const HeapVector<uint8_t> gpu = ReadStaticBuffer<uint8_t>(readback);
        EXPECT_EQ(std::memcmp(gpu.data(), scene.GetVertices().data(), scene.GetVertices().size() * sizeof(asset::Vertex)), 0);
        if (step > 0)
        {
            EXPECT_NE(std::memcmp(gpu.data(), initial.data(), initial.size() * sizeof(asset::Vertex)), 0);
            bool casterChanged = false;
            for (size_t instance = 0; instance < casters.size(); ++instance)
            {
                casterChanged |= scene.GetShadowCasters()[instance].revision != casters[instance].revision;
            }
            EXPECT_TRUE(casterChanged);
        }
        EXPECT_GT(scene.GetVoxelTriangleCount(), 0u);
        device->NextFrame();
    }
    scene.Destroy();
}
