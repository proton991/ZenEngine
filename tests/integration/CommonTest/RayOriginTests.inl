TEST_P(RayQueryIntegrationTest, NearOriginSurfaceOffsetsRejectSelfHits)
{
    if (device->GetGPUInfo().rayQuery.IsUsable())
    {
        ShaderProgramManager::GetInstance().StoreProgram(
            ZEN_NEW() ComputeFileSP(device, "RayOriginCheckSP", "VoxelGI/Calibration/ray_origin_check.comp.spv"));
        sg::Scene source;
        source.GetAABB()                    = sg::AABB(Vec3(-1), Vec3(1));
        UniquePtr<sg::Material> material    = MakeUnique<sg::Material>("origin material");
        material->index                     = 0;
        material->data.materialProperties.z = 1.0f;
        material->data.baseColorFactor      = Vec4(1);
        UniquePtr<sg::SubMesh> primitive    = MakeUnique<sg::SubMesh>("origin plane", 0, 3, 3);
        primitive->SetMaterial(0, material.Get());
        UniquePtr<sg::Mesh> mesh = MakeUnique<sg::Mesh>("origin mesh");
        mesh->SetAABB(Vec3(-1, -1, 0), Vec3(1, 1, 0));
        mesh->AddSubMesh(primitive.Get());
        HeapVector<UniquePtr<sg::Node>> nodes;
        UniquePtr<sg::Node>             node = MakeUnique<sg::Node>(0, "origin plane");
        node->SetData(0, Mat4(1));
        node->AddComponent(mesh.Get());
        source.AddRenderableNode(node.Get());
        nodes.push_back(std::move(node));
        source.SetNodes(std::move(nodes));
        source.AddComponent(std::move(primitive));
        source.AddComponent(std::move(material));
        source.AddComponent(std::move(mesh));
        asset::Vertex vertices[3]{};
        vertices[0].pos = Vec4(-1, -1, 0, 1);
        vertices[1].pos = Vec4(1, -1, 0, 1);
        vertices[2].pos = Vec4(0, 1, 0, 1);
        for (asset::Vertex& vertex : vertices)
        {
            vertex.normal = Vec4(0, 0, 1, 0);
            vertex.color  = Vec4(1);
        }
        uint32_t   indices[] = {0, 1, 2};
        sg::Camera camera(Vec3(0, 0, -1), Vec3(0), 1);
        SceneData  data{};
        data.pScene      = &source;
        data.pVertices   = vertices;
        data.numVertices = 3;
        data.pIndices    = indices;
        data.numIndices  = 3;
        data.pCamera     = &camera;
        RenderScene scene(device, data);
        scene.LoadSceneMaterials();
        scene.PrepareBuffers();
        SceneRayQuery queries(device);
        for (float coordinate : {0.0f, 1e-8f, -1e-8f, 1e-4f, -1e-4f, 0.03f, -0.03f})
        {
            SCOPED_TRACE(coordinate);
            const Vec4 offset(0, 0, coordinate, 0);
            ASSERT_TRUE(scene.SetInstanceTransform(0, glm::translate(Mat4(1), Vec3(offset))));
            ASSERT_TRUE(scene.Update());
            RenderGraph& graph = *device->GetCurrentFrameRDG();
            ASSERT_TRUE(graph.Begin());
            ASSERT_TRUE(queries.BuildRenderGraph(scene));
            RHIFlags<RHIBufferUsageFlagBits> usage;
            usage.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer);
            RHIBuffer* output = Buffer(16 * sizeof(Vec4), usage, nullptr, RHIBufferAllocateType::eGPU);
            RHIBuffer* readback =
                Buffer(16 * sizeof(Vec4), RHIFlags<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferDstBuffer), nullptr,
                       RHIBufferAllocateType::eCPURead);
            RDGComputePassDesc pass;
            pass.SetShaderProgramName("RayOriginCheckSP");
            pass.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
            pass.BindStorageBuffer("Results", output, RDGContentGuarantee::eFullWrite);
            const Vec4 gi[] = {Vec4(0, 0, 0, 1), Vec4(64), Vec4(0), Vec4(0), Vec4(0), Vec4(1, 0, 0, 0)};
            pass.BindValue("uGISettings", gi, sizeof(gi));
            pass.BindValue("uSceneData", scene.GetSceneUniformData(), sizeof(SceneUniformData));
            queries.BindInputs(pass, scene);
            graph.AddComputePass(std::move(pass)).RecordPassCommands([offset](RDGPassCmdEncoder& encoder) {
                encoder.SetPushConstants(offset);
                encoder.Dispatch(16, 1, 1);
            });
            graph.AddTransferPass("ReadOriginCheck").CopyBuffer(output, readback, {0, 0, 16 * sizeof(Vec4)});
            ASSERT_TRUE(graph.End()) << graph.GetResult().message;
            const bool submitted = device->ExecuteRenderGraph(graph);
            queries.OnRenderGraphExecuted(submitted ? RenderFrameResult::eSubmitted : RenderFrameResult::eFailed);
            ASSERT_TRUE(submitted);
            device->WaitForIdle();
            const Vec4* results = reinterpret_cast<const Vec4*>(readback->Map());
            for (uint32_t index = 0; index < 16; ++index)
            {
                EXPECT_EQ(results[index].x, 0.0f);
                EXPECT_EQ(results[index].y, 0.0f);
                EXPECT_EQ(results[index].z, 1.0f);
                EXPECT_GT(results[index].w, 0.0f);
            }
            readback->Unmap();
        }
        queries.Destroy();
    }
}
