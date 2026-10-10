TEST_F(RenderCoreTest, RayQueryRejectsUnavailableAndBudgetBeforeAllocating)
{
    sg::Scene source;
    source.GetAABB() = sg::AABB(Vec3(-1), Vec3(1));
    SceneData data{};
    data.pScene = &source;
    RenderScene   scene(device, data);
    SceneRayQuery queries(device);
    EXPECT_FALSE(queries.BuildRenderGraph(scene));
    EXPECT_STREQ(queries.GetReason(), "ray_query_features_disabled_or_unavailable");
    rhi->info.rayQuery         = {true, true, true, true, true, 256, 1024, 1024, 1024};
    const uint32_t allocations = rhi->bufferCreations;
    EXPECT_FALSE(queries.BuildRenderGraph(scene, 1));
    EXPECT_STREQ(queries.GetReason(), "acceleration_structure_budget");
    EXPECT_EQ(rhi->bufferCreations, allocations);
    EXPECT_FALSE(queries.IsReady());
    EXPECT_EQ(queries.GetGeneration(), 0u);
    queries.Destroy();
}

TEST_F(RenderCoreTest, RayQueryRejectedInstanceUploadDoesNotPublishASnapshot)
{
    rhi->info.rayQuery = {true, true, true, true, true, 256, 1024, 1024, 1024};
    sg::Scene source;
    source.GetAABB() = sg::AABB(Vec3(-1), Vec3(1));
    SceneData data{};
    data.pScene = &source;
    RenderScene   scene(device, data);
    SceneRayQuery queries(device);
    // Instance allocation succeeds; its staging-buffer allocation fails.
    rhi->failBufferCreationAt = rhi->bufferCreations + 2;
    EXPECT_FALSE(queries.BuildRenderGraph(scene));
    EXPECT_FALSE(queries.IsReady());
    EXPECT_EQ(queries.GetGeneration(), 0u);
    EXPECT_STREQ(queries.GetReason(), "scene_build_preparation_failed");
    queries.Destroy();
}

TEST_F(RenderCoreTest, RayQueryFailedExecutionInvalidatesTheRecordedGeneration)
{
    rhi->info.rayQuery   = {true, true, true, true, true, 256, 1024, 1024, 1024};
    sceneInputs.vertices = Buffer();
    sceneInputs.indices  = Buffer();
    sg::Scene source;
    source.GetAABB() = sg::AABB(Vec3(-1), Vec3(1));
    SceneData data{};
    data.pScene = &source;
    RenderScene   scene(device, data);
    SceneRayQuery queries(device);
    RenderGraph&  graph = *device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph.Begin());
    ASSERT_TRUE(queries.BuildRenderGraph(scene));
    EXPECT_EQ(queries.GetGeneration(), 1u);
    ASSERT_TRUE(graph.End()) << graph.GetResult().message;
    rhi->failSubmissionAt = rhi->submissionAttempts + 1;
    const bool succeeded  = device->ExecuteRenderGraph(graph);
    EXPECT_FALSE(succeeded);
    queries.OnRenderGraphExecuted(succeeded);
    EXPECT_FALSE(queries.IsReady());
    EXPECT_EQ(queries.GetGeneration(), 0u);
    EXPECT_STREQ(queries.GetReason(), "failed_frame");
    queries.Destroy();
    device->DestroyBuffer(sceneInputs.vertices);
    device->DestroyBuffer(sceneInputs.indices);
}

TEST_F(RenderCoreTest, RayQueryCommandCopiesDescriptionsAndRetainsIndirectResources)
{
    RHIBufferCreateInfo info;
    info.size = 256;
    info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eAccelerationStructureInput, RHIBufferUsageFlagBits::eStorageBuffer,
                             RHIBufferUsageFlagBits::eDeviceAddress);
    RHIBuffer*                         input = device->CreateBuffer(info);
    RHIAccelerationStructureCreateInfo create;
    create.size                       = 4096;
    RHIAccelerationStructure* blas    = rhi->CreateAccelerationStructure(create);
    create.type                       = RHIAccelerationStructureType::eTopLevel;
    create.referencedStructures       = MakeVecView(&blas, 1);
    RHIAccelerationStructure* tlas    = rhi->CreateAccelerationStructure(create);
    const uint64_t            inputId = input->GetStableId();
    const uint64_t            blasId  = blas->GetStableId();
    const uint64_t            tlasId  = tlas->GetStableId();
    {
        RHICommandListPtr                list(RHICommandList::Create(rhi->GetCommandContext(RHICommandContextType::eGraphics)));
        RHIAccelerationStructureGeometry geometry;
        geometry.pVertexBuffer = geometry.pIndexBuffer = input;
        geometry.vertexStride                          = 12;
        geometry.vertexCount                           = 3;
        geometry.indexCount                            = 3;
        RHIAccelerationStructureBuildInfo build;
        build.pDestination           = tlas;
        build.pScratchBuffer         = input;
        build.description.geometries = MakeVecView(&geometry, 1);
        list->BuildAccelerationStructure(build);
        geometry.indexCount = 99;
        input->ReleaseReference();
        blas->ReleaseReference();
        tlas->ReleaseReference();
        EXPECT_FALSE(destroyed.contains(inputId));
        EXPECT_FALSE(destroyed.contains(blasId));
        EXPECT_FALSE(destroyed.contains(tlasId));
        GetRHIThread().Invoke([&list] { list->Execute(); });
        ASSERT_EQ(rhi->graphics.accelerationStructureBuildCounts.size(), 1u);
        EXPECT_EQ(rhi->graphics.accelerationStructureBuildCounts[0], 3u);
    }
    EXPECT_TRUE(destroyed.contains(inputId));
    EXPECT_TRUE(destroyed.contains(blasId));
    EXPECT_TRUE(destroyed.contains(tlasId));
}
