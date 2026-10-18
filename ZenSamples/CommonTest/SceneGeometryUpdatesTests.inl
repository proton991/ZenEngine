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
    RenderScene scene(device, data);
    scene.LoadSceneMaterials();
    scene.PrepareBuffers();
    RHIBuffer* readback = Buffer(sizeof(glm::uvec4), RHIBufferAllocateType::eCPURead);
    RHIBuffer* original = scene.GetVoxelTriangleBuffer();
    EXPECT_EQ(scene.GetVoxelTriangleCount(), 4u);
    EXPECT_EQ(ReadSceneTriangleRecord(device, original, readback), glm::uvec4(0, 0, 0, GI_STATIC));

    EXPECT_TRUE(scene.SetInstanceTransform(0, glm::translate(Mat4(1), Vec3(0.5f, 0, 0))));
    EXPECT_TRUE(scene.CommitGeometryUpdates());
    EXPECT_EQ(scene.GetVoxelTriangleBuffer(), original);
    EXPECT_EQ(scene.GetAABB(), sg::AABB(Vec3(-0.5f, -1, -0.5f), Vec3(2.5f, 1, 0.5f)));
    vertices[2].pos.y = 0.75f;
    EXPECT_TRUE(scene.UpdateVertices(0, VectorView<const asset::Vertex>(vertices, 3)));
    EXPECT_TRUE(scene.CommitGeometryUpdates());
    EXPECT_EQ(scene.GetVoxelTriangleBuffer(), original);
    EXPECT_EQ(scene.GetAABB(), sg::AABB(Vec3(-0.5f, -1, -0.5f), Vec3(2.5f, 0.75f, 0.5f)));
    EXPECT_EQ(ReadSceneTriangleRecord(device, scene.GetVoxelTriangleBuffer(), readback), glm::uvec4(0, 0, 0, GI_STATIC));

    // A coverage change updates voxel geometry but keeps the same material assignment.
    sg::MaterialData material  = scene.GetMaterialsData()[0];
    material.baseColorFactor.a = 0.5f;
    EXPECT_TRUE(scene.UpdateMaterial(0, material));
    EXPECT_TRUE(scene.CommitGeometryUpdates());
    EXPECT_EQ(scene.GetVoxelTriangleBuffer(), original);

    EXPECT_TRUE(scene.SetInstanceClass(0, GI_DYNAMIC));
    EXPECT_TRUE(scene.CommitGeometryUpdates());
    EXPECT_EQ(ReadSceneTriangleRecord(device, scene.GetVoxelTriangleBuffer(), readback), glm::uvec4(0, 0, 0, GI_DYNAMIC));
    EXPECT_TRUE(scene.SetInstanceEnabled(0, false));
    EXPECT_TRUE(scene.CommitGeometryUpdates());
    EXPECT_EQ(ReadSceneTriangleRecord(device, scene.GetVoxelTriangleBuffer(), readback), glm::uvec4(0, 0, 0, 0));
    EXPECT_EQ(scene.GetAABB(), sg::AABB(Vec3(-0.5f), Vec3(2.5f, 0.5f, 0.5f)));
    EXPECT_TRUE(scene.SetMaterialVariant(0));
    EXPECT_TRUE(scene.SetInstanceEnabled(0, true));
    EXPECT_TRUE(scene.CommitGeometryUpdates());
    EXPECT_EQ(ReadSceneTriangleRecord(device, scene.GetVoxelTriangleBuffer(), readback), glm::uvec4(0, 0, 1, GI_DYNAMIC));
    EXPECT_TRUE(scene.SetMaterialVariant(-1));
    EXPECT_TRUE(scene.CommitGeometryUpdates());
    EXPECT_EQ(ReadSceneTriangleRecord(device, scene.GetVoxelTriangleBuffer(), readback), glm::uvec4(0, 0, 0, GI_DYNAMIC));

    // Authored visibility is observed by the animation path, independently of the explicit mask.
    source.GetAssetData().animations.emplace_back();
    EXPECT_TRUE(scene.SetAnimation(0));
    for (bool visible : {false, true})
    {
        node.visible = visible;
        EXPECT_TRUE(scene.AdvanceAnimation(1.0f / 60.0f));
        EXPECT_TRUE(scene.CommitGeometryUpdates());
        EXPECT_EQ(ReadSceneTriangleRecord(device, scene.GetVoxelTriangleBuffer(), readback),
                  glm::uvec4(0, 0, 0, visible ? GI_DYNAMIC : 0));
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
    scene.Destroy();
}
