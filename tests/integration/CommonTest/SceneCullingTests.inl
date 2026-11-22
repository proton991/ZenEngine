struct SceneCullCase
{
    Vec3 minimum;
    Vec3 maximum;
    bool visible;
};

SceneCullCase SceneCullFixtureCase(uint32_t index)
{
    const SceneCullCase cases[] = {
        {Vec3(-.25f, -.25f, -3.25f), Vec3(.25f, .25f, -2.75f), true},
        {Vec3(-7, -.25f, -3.25f), Vec3(-6, .25f, -2.75f), false},
        {Vec3(6, -.25f, -3.25f), Vec3(7, .25f, -2.75f), false},
        {Vec3(-.25f, -7, -3.25f), Vec3(.25f, -6, -2.75f), false},
        {Vec3(-.25f, 6, -3.25f), Vec3(.25f, 7, -2.75f), false},
        {Vec3(-.1f, -.1f, -.8f), Vec3(.1f, .1f, -.2f), false},
        {Vec3(-.25f, -.25f, -13), Vec3(.25f, .25f, -12), false},
        {Vec3(-.25f, -.25f, 1), Vec3(.25f, .25f, 2), false},
        {Vec3(-.1f, -.1f, -1.2f), Vec3(.1f, .1f, -.8f), true},
        {Vec3(3, -.25f, -3), Vec3(3.5f, .25f, -3), true},
        {Vec3(-.1f, -.1f, -1), Vec3(.1f, .1f, -1), true},
        {Vec3(-2), Vec3(2), true},
        {Vec3(std::numeric_limits<float>::quiet_NaN(), 6, -3), Vec3(7), true},
        {Vec3(6, 6, -3), Vec3(std::numeric_limits<float>::infinity(), 7, -2), true},
        {Vec3(7, 7, -2), Vec3(6, 6, -3), true},
    };
    return cases[index % 15];
}

void ExpectSceneIndirectCommand(const DrawIndexedIndirectCommand& actual, const DrawIndexedIndirectCommand& expected)
{
    EXPECT_EQ(actual.indexCount, expected.indexCount);
    EXPECT_EQ(actual.instanceCount, expected.instanceCount);
    EXPECT_EQ(actual.firstIndex, expected.firstIndex);
    EXPECT_EQ(actual.vertexOffset, expected.vertexOffset);
    EXPECT_EQ(actual.firstInstance, expected.firstInstance);
}

TEST_P(ConeVoxelGIIntegrationTest, SceneFrustumCullWritesConservativeCommandsWithinTheRequestedRange)
{
    // Three workgroups with a partial tail, plus untouched prefix/suffix guards.
    const uint32_t                               first        = 3;
    const uint32_t                               count        = 137;
    const uint32_t                               total        = first + count + 3;
    const uint32_t                               commandBytes = total * sizeof(DrawIndexedIndirectCommand);
    const DrawIndexedIndirectCommand             sentinel{0xabcdef01u, 0xabcdef02u, 0xabcdef03u, -123456, 0xabcdef04u};
    const HeapVector<DrawIndexedIndirectCommand> initial(total, sentinel);
    RHIBuffer* initialCommands = Buffer(commandBytes, RHIBufferAllocateType::eCPUWrite, initial.data());
    RHIBuffer* output          = Buffer(commandBytes, RHIBufferAllocateType::eGPU);
    RHIBuffer* readback        = Buffer(commandBytes, RHIBufferAllocateType::eCPURead);
    ASSERT_NE(initialCommands, nullptr);
    ASSERT_NE(output, nullptr);
    ASSERT_NE(readback, nullptr);

    for (uint32_t projectionType = 0; projectionType < 2; ++projectionType)
    {
        Mat4 projection   = projectionType == 0 ? glm::perspective(glm::radians(90.0f), 1.0f, 1.0f, 10.0f)
                                                : glm::ortho(-3.0f, 3.0f, -3.0f, 3.0f, 1.0f, 10.0f);
        projection[1][1] *= -1.0f;
        for (uint32_t cameraPose = 0; cameraPose < 2; ++cameraPose)
        {
            // The second camera rotates axes exactly and translates the same analytic fixture.
            Mat4 camera(1);
            if (cameraPose != 0)
            {
                camera[0] = Vec4(0, 0, -1, 0);
                camera[2] = Vec4(1, 0, 0, 0);
                camera[3] = Vec4(4, -2, 7, 1);
            }
            for (uint32_t population = 0; population < 4; ++population)
            {
                SCOPED_TRACE(testing::Message()
                             << "projection=" << projectionType << " camera=" << cameraPose << " population=" << population);
                HeapVector<GBufferDrawData> draws(total);
                HeapVector<uint32_t>        expectedVisibility(total, 0);
                for (uint32_t index = 0; index < total; ++index)
                {
                    const uint32_t      sampleIndex = population == 1 ? 0 : population == 2 ? 1 : index % 15;
                    const SceneCullCase sample      = SceneCullFixtureCase(sampleIndex);
                    Vec3                minimum     = sample.minimum;
                    Vec3                maximum     = sample.maximum;
                    if (cameraPose != 0 && sampleIndex < 12)
                    {
                        const Vec3 corner0(camera * Vec4(minimum, 1));
                        const Vec3 corner1(camera * Vec4(maximum, 1));
                        minimum = glm::min(corner0, corner1);
                        maximum = glm::max(corner0, corner1);
                    }
                    draws[index].boundsMin    = Vec4(minimum, 0);
                    draws[index].boundsMax    = Vec4(maximum, 0);
                    draws[index].geometry     = glm::uvec4(3 * (index + 1), 9 * index, 0, 0);
                    draws[index].indices      = glm::uvec4(index + 11, index + 23, 0, 0);
                    expectedVisibility[index] = sample.visible ? 1u : 0u;
                }
                RHIBuffer* input = Buffer(total * sizeof(GBufferDrawData), RHIBufferAllocateType::eCPUWrite, draws.data());
                ASSERT_NE(input, nullptr);
                FrustumCullUniformData uniform{};
                uniform.projectionView = projection * glm::inverse(camera);
                const glm::uvec4 range(first, population == 3 ? 0 : count, 0, 0);
                const uint32_t   groupCount = (count + ZEN_SCENE_CULL_GROUP_SIZE - 1) / ZEN_SCENE_CULL_GROUP_SIZE;
                const glm::uvec3 groups     = cameraPose == 0 ? glm::uvec3(1, groupCount, 1) : glm::uvec3(1, 1, groupCount);
                RenderGraph      graph("SceneFrustumCullReadback");
                ASSERT_TRUE(graph.Begin());
                graph.GetResourceManager()->ImportHostWrittenBuffer(input);
                if (population == 0)
                {
                    graph.GetResourceManager()->ImportHostWrittenBuffer(initialCommands);
                    graph.AddTransferPass("InitializeGuardCommands").CopyBuffer(initialCommands, output, {0, 0, commandBytes});
                }
                RDGComputePassDesc pass;
                pass.SetShaderProgramName("SceneFrustumCullSP");
                pass.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
                pass.BindValue("uFrustumCull", uniform);
                pass.BindStorageBuffer("GBufferDrawBuffer", input);
                pass.BindStorageBuffer("IndirectBuffer", output);
                graph.AddComputePass(std::move(pass)).RecordPassCommands([range, groups](RDGPassCmdEncoder& encoder) {
                    encoder.SetPushConstants(range);
                    encoder.Dispatch(groups.x, groups.y, groups.z);
                });
                graph.AddTransferPass("ReadSceneFrustumCommands")
                    .CopyBuffer(output, readback, {0, 0, commandBytes})
                    .NeverCull();
                ASSERT_TRUE(graph.End());
                ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
                device->FlushRHIThread();
                device->WaitForIdle();
                const HeapVector<DrawIndexedIndirectCommand> commands = ReadStaticBuffer<DrawIndexedIndirectCommand>(readback);
                ASSERT_EQ(commands.size(), total);
                for (uint32_t index = 0; index < total; ++index)
                {
                    SCOPED_TRACE(index);
                    const bool guard = index < first || index >= first + count;
                    // The empty dispatch must retain the preceding all-culled commands.
                    const DrawIndexedIndirectCommand expected =
                        guard ? sentinel
                              : DrawIndexedIndirectCommand{draws[index].geometry.x,
                                                           population == 3 ? 0u : expectedVisibility[index],
                                                           draws[index].geometry.y, 0, index};
                    ExpectSceneIndirectCommand(commands[index], expected);
                }
            }
        }
    }
}

TEST_P(ConeVoxelGIIntegrationTest, CulledGBufferSubMeshBoundsRetainCommittedGeometryAndFallbackAfterVertexEdits)
{
    sg::Scene               source;
    UniquePtr<sg::Material> material = MakeUnique<sg::Material>("bounds material");
    material->index                  = 0;
    sg::Mesh      mesh("separated submeshes");
    asset::Vertex vertices[6]{};
    vertices[0].pos = Vec4(-.25f, -.25f, -3, 1);
    vertices[1].pos = Vec4(.25f, -.25f, -3, 1);
    vertices[2].pos = Vec4(0, .25f, -3, 1);
    for (uint32_t index = 0; index < 3; ++index)
    {
        vertices[index + 3]        = vertices[index];
        vertices[index + 3].pos.x += 8;
    }
    sg::SubMesh* primitives[2]{};
    for (uint32_t index = 0; index < 2; ++index)
    {
        UniquePtr<sg::SubMesh> primitive = MakeUnique<sg::SubMesh>("separated primitive", index * 3, 3, 3);
        primitive->SetMaterial(0, material.Get());
        primitive->SetBounds(sg::AABB(Vec3(8 * index - .25f, -.25f, -3), Vec3(8 * index + .25f, .25f, -3)));
        primitives[index] = primitive.Get();
        mesh.AddSubMesh(primitive.Get());
        source.AddComponent(std::move(primitive));
    }
    source.AddComponent(std::move(material));
    mesh.SetAABB(Vec3(-.25f, -.25f, -3), Vec3(8.25f, .25f, -3));
    sg::Node node(0, "separated instance");
    node.AddComponent(&mesh);
    node.SetData(0, Mat4(1));
    source.AddRenderableNode(&node);
    source.GetAABB()         = sg::AABB(Vec3(-.25f, -.25f, -3), Vec3(8.25f, .25f, -3));
    const uint32_t indices[] = {0, 1, 2, 3, 4, 5};
    SceneData      data{};
    data.pScene      = &source;
    data.pVertices   = vertices;
    data.pIndices    = indices;
    data.numVertices = 6;
    data.numIndices  = 6;
    RenderScene scene(device, data);
    scene.LoadSceneMaterials();
    scene.PrepareBuffers();
    ASSERT_EQ(scene.GetShadowCasters().size(), 1u);
    const sg::AABB initialFirst  = scene.GetSubMeshWorldBounds(0, 0);
    const sg::AABB initialSecond = scene.GetSubMeshWorldBounds(0, 1);
    EXPECT_EQ(initialFirst, primitives[0]->GetAABB());
    EXPECT_EQ(initialSecond, primitives[1]->GetAABB());
    EXPECT_NE(initialFirst, scene.GetShadowCasters()[0].bounds);
    EXPECT_NE(initialSecond, scene.GetShadowCasters()[0].bounds);
    const Mat4 transform = glm::translate(Mat4(1), Vec3(-8, 1, 0)) * glm::scale(Mat4(1), Vec3(-2, .5f, 1));
    ASSERT_TRUE(scene.SetInstanceTransform(0, transform));
    EXPECT_EQ(scene.GetSubMeshWorldBounds(0, 0), initialFirst);
    EXPECT_EQ(scene.GetSubMeshWorldBounds(0, 1), initialSecond);
    ASSERT_TRUE(scene.CommitGeometryUpdates());
    const sg::AABB transformedFirst(Vec3(-8.5f, .875f, -3), Vec3(-7.5f, 1.125f, -3));
    const sg::AABB transformedSecond(Vec3(-24.5f, .875f, -3), Vec3(-23.5f, 1.125f, -3));
    EXPECT_EQ(scene.GetSubMeshWorldBounds(0, 0), transformedFirst);
    EXPECT_EQ(scene.GetSubMeshWorldBounds(0, 1), transformedSecond);
    vertices[0].pos.x = -5;
    ASSERT_TRUE(scene.UpdateVertices(0, MakeVecView(vertices, 1)));
    EXPECT_EQ(scene.GetSubMeshWorldBounds(0, 0), transformedFirst);
    ASSERT_TRUE(scene.CommitGeometryUpdates());
    EXPECT_EQ(scene.GetSubMeshWorldBounds(0, 0), scene.GetShadowCasters()[0].bounds);
    EXPECT_EQ(scene.GetSubMeshWorldBounds(0, 1), transformedSecond);
    EXPECT_EQ(scene.GetSubMeshWorldBounds(0, 99), scene.GetShadowCasters()[0].bounds);
    EXPECT_EQ(scene.GetSubMeshWorldBounds(99, 0), sg::AABB());
    primitives[1]->SetBounds(sg::AABB());
    ASSERT_TRUE(scene.SetInstanceTransform(0, Mat4(1)));
    ASSERT_TRUE(scene.CommitGeometryUpdates());
    EXPECT_EQ(scene.GetSubMeshWorldBounds(0, 1), scene.GetShadowCasters()[0].bounds);
    // The stale result survives a transform-only commit, and restored vertices clear it.
    EXPECT_EQ(scene.GetSubMeshWorldBounds(0, 0), scene.GetShadowCasters()[0].bounds);
    vertices[0].pos.x = -.25f;
    ASSERT_TRUE(scene.UpdateVertices(0, MakeVecView(vertices, 1)));
    ASSERT_TRUE(scene.CommitGeometryUpdates());
    EXPECT_EQ(scene.GetSubMeshWorldBounds(0, 0), primitives[0]->GetAABB());
    ASSERT_TRUE(scene.SetInstanceEnabled(0, false));
    ASSERT_TRUE(scene.CommitGeometryUpdates());
    EXPECT_EQ(scene.GetSubMeshWorldBounds(0, 0), sg::AABB());
    EXPECT_EQ(scene.GetSubMeshWorldBounds(0, 1), sg::AABB());
}
