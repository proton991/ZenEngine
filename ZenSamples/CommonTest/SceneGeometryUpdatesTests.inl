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
    scene.Destroy();
}
