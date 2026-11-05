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

TEST_F(RenderCoreTest, RayQueryRejectedMetadataUploadDoesNotPublishASnapshot)
{
    rhi->info.rayQuery = {true, true, true, true, true, 256, 1024, 1024, 1024};
    sg::Scene source;
    source.GetAABB() = sg::AABB(Vec3(-1), Vec3(1));
    SceneData data{};
    data.pScene = &source;
    RenderScene   scene(device, data);
    SceneRayQuery queries(device);
    // Metadata allocation succeeds; its staging-buffer allocation fails.
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
    queries.OnRenderGraphExecuted(succeeded ? RenderFrameResult::eSubmitted : RenderFrameResult::eFailed);
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
        build.pDestination           = blas;
        build.pScratchBuffer         = input;
        build.description.geometries = MakeVecView(&geometry, 1);
        list->BuildAccelerationStructure(build);
        RHIAccelerationStructureInstance instance;
        instance.pStructure          = blas;
        instance.customIndex         = 37;
        instance.mask                = 0x5a;
        instance.transform[1][3]     = 2.5f;
        build.pDestination           = tlas;
        build.description.type       = RHIAccelerationStructureType::eTopLevel;
        build.description.geometries = {};
        build.description.instances  = MakeVecView(&instance, 1);
        list->BuildAccelerationStructure(build);
        instance            = {};
        geometry.indexCount = 99;
        input->ReleaseReference();
        blas->ReleaseReference();
        tlas->ReleaseReference();
        EXPECT_FALSE(destroyed.contains(inputId));
        EXPECT_FALSE(destroyed.contains(blasId));
        EXPECT_FALSE(destroyed.contains(tlasId));
        GetRHIThread().Invoke([&list] { list->Execute(); });
        ASSERT_EQ(rhi->graphics.accelerationStructureBuildCounts.size(), 2u);
        EXPECT_EQ(rhi->graphics.accelerationStructureBuildCounts[0], 3u);
        EXPECT_EQ(rhi->graphics.accelerationStructureBuildCounts[1], 1u);
        ASSERT_EQ(rhi->graphics.accelerationStructureInstances.size(), 1u);
        const RHIAccelerationStructureInstance& copied = rhi->graphics.accelerationStructureInstances[0];
        EXPECT_EQ(copied.pStructure, blas);
        EXPECT_EQ(copied.customIndex, 37u);
        EXPECT_EQ(copied.mask, 0x5au);
        EXPECT_EQ(copied.transform[1][3], 2.5f);
    }
    EXPECT_TRUE(destroyed.contains(inputId));
    EXPECT_TRUE(destroyed.contains(blasId));
    EXPECT_TRUE(destroyed.contains(tlasId));
}

TEST_F(RenderCoreTest, PortableRayQueryInstancesValidateMetadataWithoutNativeAddresses)
{
    RHIRayQueryCapabilities capabilities{false, true, true, true, true, 256, 1024, 1024, 1024};
    EXPECT_TRUE(capabilities.IsUsable());
    capabilities.graphicsBuild = false;
    EXPECT_FALSE(capabilities.IsUsable());
    RHIAccelerationStructureCreateInfo info;
    info.size = 4096;
    RHIResourcePtr<RHIAccelerationStructure> blas =
        RHIResourcePtr<RHIAccelerationStructure>::Adopt(rhi->CreateAccelerationStructure(info));
    info.type = RHIAccelerationStructureType::eTopLevel;
    RHIResourcePtr<RHIAccelerationStructure> tlas =
        RHIResourcePtr<RHIAccelerationStructure>::Adopt(rhi->CreateAccelerationStructure(info));
    RHIAccelerationStructureInstance instance;
    EXPECT_FALSE(instance.IsValid());
    instance.pStructure  = blas.Get();
    instance.customIndex = 0xffffff;
    instance.mask        = 0;
    EXPECT_TRUE(instance.IsValid());
    EXPECT_EQ(blas->GetSize(), 4096u);
    instance.customIndex = 0x1000000;
    EXPECT_FALSE(instance.IsValid());
    instance.customIndex = 7;
    instance.flags.SetFlags(RHIAccelerationStructureInstanceFlagBits::eForceOpaque,
                            RHIAccelerationStructureInstanceFlagBits::eForceNonOpaque);
    EXPECT_FALSE(instance.IsValid());
    instance.flags =
        BitField<RHIAccelerationStructureInstanceFlagBits>(static_cast<RHIAccelerationStructureInstanceFlagBits>(16));
    EXPECT_FALSE(instance.IsValid());
    instance.flags           = {};
    instance.transform[1][1] = 0;
    EXPECT_FALSE(instance.HasInvertibleTransform());
    EXPECT_FALSE(instance.IsValid());
    instance.transform[1][1] = 1;
    EXPECT_TRUE(instance.HasInvertibleTransform());
    instance.transform[0][3] = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(instance.HasInvertibleTransform());
    EXPECT_FALSE(instance.IsValid());
    instance.transform[0][3] = 3.25f;
    EXPECT_TRUE(instance.IsValid());
    instance.pStructure = tlas.Get();
    EXPECT_FALSE(instance.IsValid());
}

class PortableAccelerationStructureGraphTest : public RenderCoreTest, public testing::WithParamInterface<uint32_t>
{
protected:
    void SetUp() override
    {
        InitializeDevice(nullptr, 2, (GetParam() & 1) ? RHIExecutionMode::eThreaded : RHIExecutionMode::eInline,
                         (GetParam() & 2) ? AsyncComputeMode::eAuto : AsyncComputeMode::eDisabled, {true, true, {0, 1, 2}});
        RDGMetricsOptions options;
        options.logging.enabled      = true;
        options.logging.sampleEvery  = 1;
        options.logging.minInterval  = std::chrono::milliseconds(0);
        options.includeTransferNodes = true;
        device->GetRDGMetrics().Configure(options);
        device->GetRDGMetrics().SetSink({});
        RHIShaderCreateInfo shader;
        shader.stageFlags.SetFlag(RHIShaderStageFlagBits::eCompute);
        shader.spirvFileName[ToUnderlying(RHIShaderStage::eCompute)] = "VoxelGI/Calibration/ray_query_check.comp.spv";
        reflectedShaderInfos["PortableRayQuery"]                     = shader;
        CreateTestShaderProgram(device, "PortableRayQuery");
    }

    void Query(RenderGraph& graph, RHIAccelerationStructure* structure, RHIBuffer* output)
    {
        RDGComputePassDesc pass;
        pass.SetShaderProgramName("PortableRayQuery");
        pass.SetPassTag("Query");
        pass.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
        pass.BindAccelerationStructure("testScene", structure);
        pass.BindStorageBuffer("Results", output, RDGContentGuarantee::eFullWrite);
        graph.AddComputePass(pass).RecordPassCommands([](RDGPassCmdEncoder& encoder) { encoder.Dispatch(4, 1, 1); });
    }
};

TEST_P(PortableAccelerationStructureGraphTest, BuildQueryAndRefitTrackNativeStructures)
{
    RHIBufferCreateInfo bufferInfo;
    bufferInfo.size = 256;
    bufferInfo.usageFlags.SetFlags(RHIBufferUsageFlagBits::eAccelerationStructureInput, RHIBufferUsageFlagBits::eStorageBuffer);
    RHIResourcePtr<RHIBuffer>          input   = RHIResourcePtr<RHIBuffer>::Adopt(device->CreateBuffer(bufferInfo));
    RHIResourcePtr<RHIBuffer>          scratch = RHIResourcePtr<RHIBuffer>::Adopt(device->CreateBuffer(bufferInfo));
    RHIResourcePtr<RHIBuffer>          output  = RHIResourcePtr<RHIBuffer>::Adopt(device->CreateBuffer(bufferInfo));
    RHIAccelerationStructureCreateInfo create;
    create.size = 4096;
    RHIResourcePtr<RHIAccelerationStructure> blas =
        RHIResourcePtr<RHIAccelerationStructure>::Adopt(rhi->CreateAccelerationStructure(create));
    RHIAccelerationStructure* dependency = blas.Get();
    create.type                          = RHIAccelerationStructureType::eTopLevel;
    create.referencedStructures          = MakeVecView(&dependency, 1);
    RHIResourcePtr<RHIAccelerationStructure> tlas =
        RHIResourcePtr<RHIAccelerationStructure>::Adopt(rhi->CreateAccelerationStructure(create));
    RenderGraph graph("PortableInstances");
    ASSERT_TRUE(graph.Begin());
    graph.GetResourceManager()->ImportHostWrittenBuffer(input.Get());
    RHIAccelerationStructureGeometry geometry;
    geometry.pVertexBuffer = geometry.pIndexBuffer = input.Get();
    geometry.vertexStride                          = 12;
    geometry.vertexCount = geometry.indexCount = 3;
    RHIAccelerationStructureBuildInfo build;
    build.pDestination            = blas.Get();
    build.pScratchBuffer          = scratch.Get();
    build.description.geometries  = MakeVecView(&geometry, 1);
    build.description.allowUpdate = true;
    graph.AddTransferPass("BuildBLAS").BuildAccelerationStructure(build);
    RHIAccelerationStructureInstance instance;
    instance.pStructure          = blas.Get();
    instance.customIndex         = 73;
    instance.mask                = 0x36;
    instance.transform[2][3]     = 4.5f;
    build.pDestination           = tlas.Get();
    build.description.type       = RHIAccelerationStructureType::eTopLevel;
    build.description.geometries = {};
    build.description.instances  = MakeVecView(&instance, 1);
    graph.AddTransferPass("BuildTLAS").BuildAccelerationStructure(build);
    Query(graph, tlas.Get(), output.Get());
    geometry.indexCount = 99;
    instance            = {};
    ASSERT_TRUE(graph.End()) << graph.GetResult().message;
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    GetRHIThread().Invoke([] {});
    ASSERT_EQ(rhi->graphics.accelerationStructureBuildCounts.size(), 2u);
    EXPECT_EQ(rhi->graphics.accelerationStructureBuildCounts[0], 3u);
    EXPECT_EQ(rhi->graphics.accelerationStructureBuildCounts[1], 1u);
    ASSERT_EQ(rhi->graphics.accelerationStructureInstances.size(), 1u);
    const RHIAccelerationStructureInstance& copied = rhi->graphics.accelerationStructureInstances[0];
    EXPECT_EQ(copied.pStructure, blas.Get());
    EXPECT_EQ(copied.customIndex, 73u);
    EXPECT_EQ(copied.mask, 0x36u);
    EXPECT_EQ(copied.transform[2][3], 4.5f);
    EXPECT_TRUE(graph.GetWarnings().empty());
    ASSERT_FALSE(rhi->graphics.memoryBarriers.empty());
    EXPECT_TRUE(rhi->graphics.memoryBarriers[0].srcAccess.HasFlag(RHIAccessFlagBits::eAccelerationStructureWrite));
    EXPECT_TRUE(rhi->graphics.memoryBarriers[0].dstAccess.HasFlag(RHIAccessFlagBits::eAccelerationStructureRead));
    const RDGMetricsSnapshot& first = device->GetRDGMetrics().GetLastSnapshot();
    EXPECT_TRUE(first.validated);
    EXPECT_GE(first.totals.memoryTransitions, 1u);
    for (RDGMetricIssue issue :
         {RDGMetricIssue::eMissingBarrier, RDGMetricIssue::eStageCoverage, RDGMetricIssue::eAccessCoverage})
    {
        EXPECT_EQ(first.issues[size_t(issue)], 0u);
    }
    ResourceStateTracker& tracker = RDGSubmissionTestAccess::Tracker(*device);
    for (RHIAccelerationStructure* structure : {blas.Get(), tlas.Get()})
    {
        EXPECT_EQ(tracker.GetAccelerationStructureState(structure).accessMode, RHIAccessMode::eRead);
        EXPECT_TRUE(
            tracker.GetAccelerationStructureState(structure).pipelineStages.HasFlag(RHIPipelineStageFlagBits::eComputeShader));
        EXPECT_EQ(tracker.GetContents(structure).status, RDGContentStatus::eDefined);
        EXPECT_NE(RDGSubmissionTestAccess::History(*device).Find(structure->GetStableId()), nullptr);
    }
    EXPECT_EQ(rhi->compute.dispatchCount, (GetParam() & 2) ? 1u : 0u);

    // The next graph refits an indirectly read BLAS while keeping its TLAS. Its
    // graphics build must follow the previous query, including on another queue.
    RenderGraph refit("RefitNativeStructure");
    ASSERT_TRUE(refit.Begin());
    geometry.indexCount = 3;
    build.pDestination = build.pSource = blas.Get();
    build.description.type             = RHIAccelerationStructureType::eBottomLevel;
    build.description.instances        = {};
    build.description.geometries       = MakeVecView(&geometry, 1);
    refit.AddTransferPass("RefitBLAS").BuildAccelerationStructure(build);
    Query(refit, tlas.Get(), output.Get());
    ASSERT_TRUE(refit.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(refit));
    GetRHIThread().Invoke([] {});
    EXPECT_TRUE(refit.GetWarnings().empty());
    ASSERT_EQ(rhi->graphics.accelerationStructureBuildCounts.size(), 3u);
    EXPECT_EQ(rhi->graphics.accelerationStructureBuildCounts.back(), 3u);
    const RDGMetricsSnapshot& second = device->GetRDGMetrics().GetLastSnapshot();
    EXPECT_EQ(second.graph, NameID("RefitNativeStructure"));
    EXPECT_TRUE(second.validated);
    for (RDGMetricIssue issue :
         {RDGMetricIssue::eMissingBarrier, RDGMetricIssue::eStageCoverage, RDGMetricIssue::eAccessCoverage})
    {
        EXPECT_EQ(second.issues[size_t(issue)], 0u);
    }
    if (GetParam() & 2)
    {
        bool waitedForPriorQuery = false;
        for (const RDGSubmissionMetrics& submission : second.submissions)
        {
            if (submission.queue == RHICommandContextType::eGraphics)
            {
                for (const RDGSubmissionDependencyMetrics& predecessor : submission.dependencies)
                {
                    waitedForPriorQuery |= predecessor.external && predecessor.semaphore
                                        && predecessor.producerQueue == RHICommandContextType::eAsyncCompute;
                }
            }
        }
        EXPECT_TRUE(waitedForPriorQuery);
    }
}

// Binding a TLAS reads every BLAS it references. Memory barriers are global, so a pass needs one
// entry however many structures it touches: one per structure cost New Sponza 4.8 ms a frame.
TEST_P(PortableAccelerationStructureGraphTest, StructuresShareOneMemoryBarrierPerBatch)
{
    RHIBufferCreateInfo bufferInfo;
    bufferInfo.size = 256;
    bufferInfo.usageFlags.SetFlags(RHIBufferUsageFlagBits::eAccelerationStructureInput, RHIBufferUsageFlagBits::eStorageBuffer);
    RHIResourcePtr<RHIBuffer>          input   = RHIResourcePtr<RHIBuffer>::Adopt(device->CreateBuffer(bufferInfo));
    RHIResourcePtr<RHIBuffer>          scratch = RHIResourcePtr<RHIBuffer>::Adopt(device->CreateBuffer(bufferInfo));
    RHIResourcePtr<RHIBuffer>          output  = RHIResourcePtr<RHIBuffer>::Adopt(device->CreateBuffer(bufferInfo));
    RHIAccelerationStructureCreateInfo create;
    create.size = 4096;
    HeapVector<RHIResourcePtr<RHIAccelerationStructure>> owners;
    HeapVector<RHIAccelerationStructure*>                structures;
    HeapVector<RHIAccelerationStructureInstance>         instances;
    for (uint32_t i = 0; i < 8; ++i)
    {
        owners.push_back(RHIResourcePtr<RHIAccelerationStructure>::Adopt(rhi->CreateAccelerationStructure(create)));
        structures.push_back(owners.back().Get());
        RHIAccelerationStructureInstance instance;
        instance.pStructure = structures.back();
        instances.push_back(instance);
    }
    create.type                 = RHIAccelerationStructureType::eTopLevel;
    create.referencedStructures = MakeVecView(structures);
    RHIResourcePtr<RHIAccelerationStructure> tlas =
        RHIResourcePtr<RHIAccelerationStructure>::Adopt(rhi->CreateAccelerationStructure(create));
    RenderGraph graph("SharedMemoryBarrier");
    ASSERT_TRUE(graph.Begin());
    graph.GetResourceManager()->ImportHostWrittenBuffer(input.Get());
    RHIAccelerationStructureGeometry geometry;
    geometry.pVertexBuffer = geometry.pIndexBuffer = input.Get();
    geometry.vertexStride                          = 12;
    geometry.vertexCount = geometry.indexCount = 3;
    RHIAccelerationStructureBuildInfo build;
    build.pScratchBuffer         = scratch.Get();
    build.description.geometries = MakeVecView(&geometry, 1);
    for (RHIAccelerationStructure* structure : structures)
    {
        build.pDestination = structure;
        graph.AddTransferPass("BuildBLAS").BuildAccelerationStructure(build);
    }
    build.pDestination           = tlas.Get();
    build.description.type       = RHIAccelerationStructureType::eTopLevel;
    build.description.geometries = {};
    build.description.instances  = MakeVecView(instances);
    graph.AddTransferPass("BuildTLAS").BuildAccelerationStructure(build);
    Query(graph, tlas.Get(), output.Get());
    ASSERT_TRUE(graph.End()) << graph.GetResult().message;
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    GetRHIThread().Invoke([] {});
    const RDGMetricsSnapshot& metrics = device->GetRDGMetrics().GetLastSnapshot();
    EXPECT_TRUE(metrics.validated);
    EXPECT_GE(metrics.totals.memoryTransitions, 1u);
    ASSERT_FALSE(metrics.nodes.empty());
    for (const RDGNodeMetrics& node : metrics.nodes)
    {
        EXPECT_LE(node.memoryTransitions, 1u) << node.name.CStr();
    }
    // The merged entry must still protect every structure it stands for.
    for (RDGMetricIssue issue :
         {RDGMetricIssue::eMissingBarrier, RDGMetricIssue::eStageCoverage, RDGMetricIssue::eAccessCoverage})
    {
        EXPECT_EQ(metrics.issues[size_t(issue)], 0u);
    }
}

INSTANTIATE_TEST_SUITE_P(ExecutionAndQueueModes, PortableAccelerationStructureGraphTest, testing::Range(0u, 4u));

TEST_F(RenderCoreTest, RayQueryGeometryRequestsAddressesOnlyWhenSupported)
{
    rhi->info.rayQuery = {false, true, true, true, true, 256, 1024, 1024, 1024};
    for (bool addresses : {false, true})
    {
        rhi->info.rayQuery.bufferDeviceAddress = addresses;
        RHIBuffer* vertex                      = device->CreateVertexBuffer(48, nullptr);
        RHIBuffer* index                       = device->CreateIndexBuffer(12, nullptr);
        ASSERT_NE(vertex, nullptr);
        ASSERT_NE(index, nullptr);
        for (RHIBuffer* buffer : {vertex, index})
        {
            EXPECT_TRUE(buffer->GetUsageFlags().HasFlag(RHIBufferUsageFlagBits::eAccelerationStructureInput));
            EXPECT_EQ(buffer->GetUsageFlags().HasFlag(RHIBufferUsageFlagBits::eDeviceAddress), addresses);
            device->DestroyBuffer(buffer);
        }
    }
}

TEST_F(RenderCoreTest, NativeInstanceUploadsAreBudgetedUntilSubmissionRetires)
{
    rhi->info.rayQuery   = {false, true, true, true, true, 256, 1024, 1024, 1024};
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
    const uint64_t                    prepared = queries.GetMemoryBytes();
    RHIAccelerationStructureBuildDesc empty;
    empty.type                   = RHIAccelerationStructureType::eTopLevel;
    const uint64_t instanceBytes = rhi->GetAccelerationStructureBuildSizes(empty).instanceDataSize;
    ASSERT_GT(instanceBytes, 0u);
    ASSERT_TRUE(graph.End()) << graph.GetResult().message;
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    queries.OnRenderGraphExecuted(RenderFrameResult::eSubmitted);
    EXPECT_EQ(queries.GetMemoryBytes(), prepared);
    device->WaitForIdle();
    ASSERT_TRUE(graph.Begin());
    ASSERT_TRUE(queries.BuildRenderGraph(scene));
    EXPECT_EQ(queries.GetMemoryBytes(), prepared - instanceBytes);
    queries.Destroy();
    device->DestroyBuffer(sceneInputs.vertices);
    device->DestroyBuffer(sceneInputs.indices);
}

namespace
{
class SceneRayQueryCacheTest : public RenderCoreTest, public testing::WithParamInterface<uint32_t>
{
protected:
    void SetUp() override
    {
        InitializeDevice(nullptr, (GetParam() & 3) + 1,
                         (GetParam() & 4) != 0 ? RHIExecutionMode::eThreaded : RHIExecutionMode::eInline,
                         AsyncComputeMode::eDisabled, {false, true, {0, 1, 2}}, {false, true, true, {1, 1, 1}},
                         {true, true, true, true, true, 256, 1024, 1024, 1024});
        sceneInputs.vertices = Buffer();
        sceneInputs.indices  = Buffer();
    }

    void TearDown() override
    {
        device->DestroyBuffer(sceneInputs.vertices);
        device->DestroyBuffer(sceneInputs.indices);
        RenderCoreTest::TearDown();
    }
};

TEST_P(SceneRayQueryCacheTest, DeclinedCandidatesReuseStorageAndRemainBudgeted)
{
    sg::Scene source;
    source.GetAABB() = sg::AABB(Vec3(-1), Vec3(1));
    SceneData data{};
    data.pScene = &source;
    RenderScene                       scene(device, data);
    SceneRayQuery                     queries(device);
    const uint32_t                    slots = GRenderFrameState.GetNumFramesInFlight();
    RHIAccelerationStructureBuildDesc empty;
    empty.type                     = RHIAccelerationStructureType::eTopLevel;
    const uint64_t instanceBytes   = rhi->GetAccelerationStructureBuildSizes(empty).instanceDataSize;
    uint64_t       warmedBytes     = 0;
    uint32_t       warmedCreations = 0;
    for (uint32_t frame = 0; frame < slots * 3; ++frame)
    {
        SCOPED_TRACE(frame);
        RenderGraph& graph = *device->GetCurrentFrameRDG();
        ASSERT_TRUE(graph.Begin());
        const uint64_t uploads = device->GetStagedUploadBytes();
        ASSERT_TRUE(queries.BuildRenderGraph(scene, frame >= slots ? warmedBytes + instanceBytes : 0));
        ASSERT_TRUE(graph.End());
        queries.OnRenderGraphExecuted(RenderFrameResult::eDeclined);
        EXPECT_FALSE(queries.IsReady());
        EXPECT_EQ(queries.GetGeneration(), 0u);
        // Preparation may flush uploads before a frame is declined. The cached
        // payload remains valid, without pretending that its TLAS was committed.
        RenderGraph uploadsGraph("candidate_uploads");
        ASSERT_TRUE(uploadsGraph.Begin());
        ASSERT_TRUE(uploadsGraph.End());
        ASSERT_TRUE(device->ExecuteRenderGraph(uploadsGraph));
        device->WaitForPreviousFrames();
        if (frame >= slots)
        {
            EXPECT_EQ(rhi->bufferCreations, warmedCreations);
            EXPECT_EQ(device->GetStagedUploadBytes(), uploads);
            EXPECT_EQ(queries.GetMemoryBytes(), warmedBytes);
        }
        if (frame == slots - 1)
        {
            warmedBytes     = queries.GetMemoryBytes();
            warmedCreations = rhi->bufferCreations;
        }
        device->NextFrame();
    }
    EXPECT_GT(warmedBytes, 0u);
    EXPECT_FALSE(queries.BuildRenderGraph(scene, warmedBytes - 1));
    EXPECT_STREQ(queries.GetReason(), "acceleration_structure_budget");
    EXPECT_EQ(rhi->bufferCreations, warmedCreations);
    queries.Destroy();
    device->WaitForIdle();
    // Prune completed retirements before the tiny budget rejects a new snapshot.
    EXPECT_FALSE(queries.BuildRenderGraph(scene, 1));
    EXPECT_EQ(queries.GetMemoryBytes(), 0u);
    queries.Destroy();
}

INSTANTIATE_TEST_SUITE_P(FrameSlotsAndExecutionModes, SceneRayQueryCacheTest, testing::Range(0u, 8u));
} // namespace
