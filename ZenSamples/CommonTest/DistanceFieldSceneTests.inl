// Included in the four-mode native Vulkan suite (RayQueryIntegrationTests.cpp).
// Adds one triangle with per-corner vertex alpha to a test mesh.
void AddFieldTestTriangle(sg::Scene&                 source,
                          sg::Mesh&                  mesh,
                          sg::Material&              material,
                          HeapVector<asset::Vertex>& vertices,
                          HeapVector<uint32_t>&      indices,
                          const Vec3&                a,
                          const Vec3&                b,
                          const Vec3&                c,
                          const Vec3&                alpha = Vec3(1))
{
    UniquePtr<sg::SubMesh> primitive =
        MakeUnique<sg::SubMesh>("field test triangle", static_cast<uint32_t>(indices.size()), 3, 3);
    primitive->SetMaterial(material.index, &material);
    mesh.AddSubMesh(primitive.Get());
    source.AddComponent(std::move(primitive));
    const Vec3 positions[] = {a, b, c};
    for (uint32_t corner = 0; corner < 3; ++corner)
    {
        asset::Vertex vertex{};
        vertex.pos    = Vec4(positions[corner], 1);
        vertex.normal = Vec4(glm::normalize(glm::cross(b - a, c - a)), 0);
        vertex.color  = Vec4(1, 1, 1, alpha[corner]);
        indices.push_back(static_cast<uint32_t>(vertices.size()));
        vertices.push_back(vertex);
    }
}
struct BuiltFieldRay
{
    Vec4       originMinimum;
    Vec4       directionMaximum;
    glm::uvec4 limits{0};
};

struct BuiltFieldResult
{
    Vec4       positionDistance;
    Vec4       normal;
    glm::uvec4 statistics;
};
static_assert(sizeof(BuiltFieldRay) == 48);
static_assert(sizeof(BuiltFieldResult) == 48);

void RegisterDistanceFieldPrograms(RenderDevice* device)
{
    const char* programs[][2] = {{"DistanceFieldSnapshotSP", "distance_field_snapshot"},
                                 {"DistanceFieldSnapshotVerticesSP", "distance_field_snapshot_vertices"},
                                 {"DistanceFieldClearSP", "distance_field_clear"},
                                 {"DistanceFieldRetainSP", "distance_field_retain"},
                                 {"DistanceFieldRecycleSP", "distance_field_recycle"},
                                 {"DistanceFieldPrepareSP", "distance_field_prepare"},
                                 {"DistanceFieldAdvanceSP", "distance_field_advance"},
                                 {"DistanceFieldBatchClearSP", "distance_field_batch_clear"},
                                 {"DistanceFieldJobsSP", "distance_field_jobs"},
                                 {"DistanceFieldCoverageSP", "distance_field_coverage"},
                                 {"DistanceFieldPrepareJobsSP", "distance_field_prepare_jobs"},
                                 {"DistanceFieldCountSP", "distance_field_count"},
                                 {"DistanceFieldScanSP", "distance_field_scan"},
                                 {"DistanceFieldScanSumsSP", "distance_field_scan_sums"},
                                 {"DistanceFieldAllocateSP", "distance_field_allocate"},
                                 {"DistanceFieldCompactSP", "distance_field_compact"},
                                 {"DistanceFieldEmitSP", "distance_field_emit"},
                                 {"DistanceFieldHierarchySP", "distance_field_hierarchy"},
                                 {"DistanceFieldCheckSP", "Calibration/distance_field_check"},
                                 {"DistanceFieldCompositeCheckSP", "Calibration/distance_field_composite_check"}};
    for (const char* const(&program)[2] : programs)
    {
        ShaderProgramManager::GetInstance().StoreProgram(
            ZEN_NEW() ComputeFileSP(device, program[0], std::string("VoxelGI/") + program[1] + ".comp.spv"));
    }
}

struct BuiltFieldSceneFixture
{
    sg::Scene                 source;
    sg::Camera                camera{Vec3(0, 0, -5), Vec3(0), 1};
    HeapVector<asset::Vertex> vertices;
    HeapVector<uint32_t>      indices;
    UniquePtr<RenderScene>    scene;

    ~BuiltFieldSceneFixture()
    {
        if (scene)
        {
            scene->Destroy();
        }
    }

    // One card at x = 0.75 outside the receiver cache, optionally slanted.
    bool Init(RenderDevice* device, bool slanted = true, const Vec3& alpha = Vec3(1), bool masked = false)
    {
        Vec3 corners[] = {Vec3(0.75f, -0.6f, -0.6f), Vec3(0.75f, 0.6f, -0.6f), Vec3(0.75f, 0.6f, 0.6f),
                          Vec3(0.75f, -0.6f, 0.6f)};
        if (slanted)
        {
            for (Vec3& corner : corners)
            {
                corner.x += 0.35f * corner.y + 0.2f * corner.z;
            }
        }
        const HeapVector<Vec3> triangles = {corners[0], corners[1], corners[2], corners[0], corners[2], corners[3]};
        return InitTriangles(device, triangles, sg::AABB(Vec3(-1), Vec3(1.5f)), alpha, masked);
    }

    // One instance of the given triangles (three corners each) inside the mesh bounds.
    bool InitTriangles(RenderDevice*           device,
                       const HeapVector<Vec3>& triangles,
                       const sg::AABB&         bounds,
                       const Vec3&             alpha  = Vec3(1),
                       bool                    masked = false)
    {
        UniquePtr<sg::Material> material   = MakeUnique<sg::Material>("distance field test surface");
        material->index                    = 0;
        material->data.surfaceProperties.y = masked ? 1.0f : 0.0f;
        UniquePtr<sg::Mesh> mesh           = MakeUnique<sg::Mesh>("distance field test card");
        mesh->SetAABB(bounds.GetMin(), bounds.GetMax());
        for (size_t corner = 0; corner + 2 < triangles.size(); corner += 3)
        {
            AddFieldTestTriangle(source, *mesh, *material, vertices, indices, triangles[corner], triangles[corner + 1],
                                 triangles[corner + 2], alpha);
        }
        UniquePtr<sg::Node> node = MakeUnique<sg::Node>(0, "distance field test instance");
        node->SetData(0, Mat4(1));
        node->AddComponent(mesh.Get());
        source.AddRenderableNode(node.Get());
        HeapVector<UniquePtr<sg::Node>> nodes;
        nodes.push_back(std::move(node));
        source.SetNodes(std::move(nodes));
        source.AddComponent(std::move(mesh));
        source.AddComponent(std::move(material));
        // This receiver cache intentionally does not contain the card. Visibility must.
        source.GetAABB() = sg::AABB(Vec3(-0.25f), Vec3(0.25f));
        asset::PrepareScenePlacement(source, asset::ScenePlacement::PreserveAuthored);
        SceneData data{};
        data.pScene      = &source;
        data.pCamera     = &camera;
        data.pVertices   = vertices.data();
        data.numVertices = static_cast<uint32_t>(vertices.size());
        data.pIndices    = indices.data();
        data.numIndices  = static_cast<uint32_t>(indices.size());
        scene            = MakeUnique<RenderScene>(device, data);
        scene->LoadSceneMaterials();
        scene->PrepareBuffers();
        // Construction recomputes the authored scene AABB before capturing cache bounds.
        const bool valid = scene->SetVoxelBounds(sg::AABB(Vec3(-0.25f), Vec3(0.25f)));
        return valid && scene->Update();
    }
};

bool SubmitBuiltFieldFrame(RenderDevice& device, RenderScene& scene, DistanceFieldScene& field)
{
    RenderGraph& graph = *device.GetCurrentFrameRDG();
    bool         valid = graph.Begin();
    if (valid)
    {
        field.BuildRenderGraph(scene);
        valid = graph.End();
        EXPECT_TRUE(valid) << graph.GetResult().message;
        if (valid)
        {
            valid = device.ExecuteRenderGraph(graph);
            EXPECT_TRUE(valid) << graph.GetResult().message;
        }
        field.OnRenderGraphExecuted(valid ? RenderFrameResult::eSubmitted : RenderFrameResult::eFailed);
        // Test readback only. The production owner never flushes or waits for GPU completion.
        device.FlushRHIThread();
        device.WaitForIdle();
    }
    return valid;
}

// Frames until the field fails or publishes a generation of the committed geometry. After an edit the
// previous generation stays published, so Ready alone does not mean the edit has been built.
bool CompleteBuiltField(RenderDevice& device, RenderScene& scene, DistanceFieldScene& field, uint32_t frameLimit = 512)
{
    bool     valid    = true;
    uint32_t frames   = 0;
    bool     complete = field.GetState() == DistanceFieldScene::State::Failed || field.IsCurrent(scene);
    while (valid && !complete && frames < frameLimit)
    {
        valid    = SubmitBuiltFieldFrame(device, scene, field);
        complete = field.GetState() == DistanceFieldScene::State::Failed || field.IsCurrent(scene);
        ++frames;
    }
    EXPECT_TRUE(complete) << "Frame limit: " << frameLimit;
    EXPECT_LE(field.GetPersistentBytes(), 256ull * 1024 * 1024);
    EXPECT_LE(field.GetScratchBytes(), 128ull * 1024 * 1024);
    return valid && complete;
}

// Traces the scene field alone, or as hybrid GI traces it: with local volumes and stale regions.
HeapVector<BuiltFieldResult> TraceBuiltField(RenderDevice&                    device,
                                             DistanceFieldScene&              field,
                                             const HeapVector<BuiltFieldRay>& rays,
                                             bool                             composite = false)
{
    HeapVector<BuiltFieldResult> results;
    RHIBufferCreateInfo          info;
    info.size = rays.size() * sizeof(BuiltFieldRay);
    info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eStorageBuffer);
    info.allocateType = RHIBufferAllocateType::eCPUWrite;
    RHIBuffer* input  = device.CreateBuffer(info);
    info.size         = rays.size() * sizeof(BuiltFieldResult);
    info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferSrcBuffer);
    info.allocateType   = RHIBufferAllocateType::eGPU;
    RHIBuffer* output   = device.CreateBuffer(info);
    info.usageFlags     = BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferDstBuffer);
    info.allocateType   = RHIBufferAllocateType::eCPURead;
    RHIBuffer* readback = device.CreateBuffer(info);
    bool       valid    = input != nullptr && output != nullptr && readback != nullptr;
    uint8_t*   mapped   = valid ? input->Map() : nullptr;
    valid               = valid && mapped != nullptr;
    if (valid)
    {
        std::memcpy(mapped, rays.data(), rays.size() * sizeof(BuiltFieldRay));
        input->Unmap();
        RenderGraph& graph = *device.GetCurrentFrameRDG();
        valid              = graph.Begin();
        if (valid)
        {
            graph.GetResourceManager()->ImportHostWrittenBuffer(input);
            RDGComputePassDesc pass;
            pass.SetShaderProgramName(composite ? "DistanceFieldCompositeCheckSP" : "DistanceFieldCheckSP");
            pass.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
            valid = composite ? field.BindCompositeInputs(pass) : field.BindTraceInputs(pass);
            pass.BindStorageBuffer("DistanceFieldCheckRays", input);
            pass.BindStorageBuffer("DistanceFieldCheckResults", output, RDGContentGuarantee::eFullWrite);
            const uint32_t groups = (static_cast<uint32_t>(rays.size()) + 63) / 64;
            graph.AddComputePass(std::move(pass)).RecordPassCommands([groups](RDGPassCmdEncoder& encoder) {
                encoder.Dispatch(groups, 1, 1);
            });
            graph.AddTransferPass("ReadConstructedFieldTrace").CopyBuffer(output, readback, {0, 0, info.size}).NeverCull();
            valid = graph.End() && valid;
            if (valid)
            {
                valid = device.ExecuteRenderGraph(graph);
            }
            EXPECT_TRUE(valid) << graph.GetResult().message;
            device.FlushRHIThread();
            device.WaitForIdle();
        }
        mapped = valid ? readback->Map() : nullptr;
        valid  = valid && mapped != nullptr;
        if (valid)
        {
            results.resize(rays.size());
            std::memcpy(results.data(), mapped, results.size() * sizeof(BuiltFieldResult));
            readback->Unmap();
        }
    }
    EXPECT_TRUE(valid);
    device.DestroyBuffer(input);
    device.DestroyBuffer(output);
    device.DestroyBuffer(readback);
    return results;
}

DistanceFieldSceneSettings SmallFieldSettings()
{
    DistanceFieldSceneSettings settings;
    settings.cellsPerAxis       = 16;
    settings.atlasTexelsPerAxis = 96; // Non-power-of-two packing is part of the production ABI.
    settings.triangleBatchSize  = 1;  // Forces min-distance accumulation across independent triangle batches.
    settings.cellBatchSize      = 32;
    settings.referenceCapacity  = 16384;
    return settings;
}

TEST_P(DistanceFieldSceneIntegrationTest, DistanceFieldBuildSlantedSurfaceOutsideRadianceCache)
{
    RegisterDistanceFieldPrograms(device);
    BuiltFieldSceneFixture fixture;
    ASSERT_TRUE(fixture.Init(device));
    EXPECT_EQ(fixture.scene->GetVoxelSceneBounds().GetMax(), Vec3(0.25f));
    for (const asset::Vertex& vertex : fixture.vertices)
    {
        EXPECT_GT(vertex.pos.x, fixture.scene->GetVoxelSceneBounds().GetMax().x);
        EXPECT_LE(vertex.pos.x, fixture.scene->GetAABB().GetMax().x);
    }
    DistanceFieldScene field(device, SmallFieldSettings());
    ASSERT_TRUE(CompleteBuiltField(*device, *fixture.scene, field));
    ASSERT_EQ(field.GetState(), DistanceFieldScene::State::Ready) << field.GetFailureReason();
    EXPECT_GT(field.GetSubmittedGraphCount(), 0u);
    EXPECT_GT(field.GetSnapshotBytes(), 0u);
    EXPECT_EQ(field.GetScratchBytes(), 0u);
    EXPECT_GT(field.GetUniform().gridMinCellSize.x + field.GetUniform().gridMinCellSize.w * 16,
              fixture.scene->GetVoxelSceneBounds().GetMax().x);
    const Vec3                      normal = glm::normalize(Vec3(1, -0.35f, -0.2f));
    const Vec3                      center(0.75f, 0, 0);
    const HeapVector<BuiltFieldRay> rays = {
        {Vec4(center - normal, 0), Vec4(normal, 4)},
        {Vec4(center + normal, 0), Vec4(-normal, 4)},
        {Vec4(center - normal, 0), Vec4(normal, 0.5f)},
        {Vec4(-2, 2, 2, 0), Vec4(1, 0, 0, 4)},
    };
    const HeapVector<BuiltFieldResult> results = TraceBuiltField(*device, field, rays);
    ASSERT_EQ(results.size(), rays.size());
    for (uint32_t index = 0; index < 2; ++index)
    {
        SCOPED_TRACE(index);
        EXPECT_EQ(results[index].statistics.x, 1u);
        // Reports construction error against an analytic slanted plane; this broad fixture
        // tolerance establishes geometry/data flow, not the lantern or thin-slit quality gate.
        EXPECT_NEAR(results[index].positionDistance.w, 1.0f, field.GetUniform().gridMinCellSize.w * 0.3f);
        EXPECT_LT(glm::dot(Vec3(results[index].normal), Vec3(rays[index].directionMaximum)), -0.8f)
            << "normal " << results[index].normal.x << ", " << results[index].normal.y << ", " << results[index].normal.z
            << "; hit distance " << results[index].positionDistance.w;
    }
    EXPECT_EQ(results[2].statistics.x, 0u);
    EXPECT_EQ(results[3].statistics.x, 0u);
}

TEST_P(DistanceFieldSceneIntegrationTest, DistanceFieldCollapsedAndFiniteSliverTriangles)
{
    RegisterDistanceFieldPrograms(device);
    BuiltFieldSceneFixture fixture;
    ASSERT_TRUE(fixture.Init(device, false));
    // Actual New Sponza failure 892812: B and C collapse to the same grid position.
    // The second triangle remains finite, with an aspect ratio above one million.
    const Vec3  gridPositions[] = {Vec3(47.4250908f, 22.1404972f, 17.1420593f),
                                   Vec3(47.4250908f, 22.1396427f, 17.0475636f),
                                   Vec3(47.4250908f, 22.1396427f, 17.0475636f),
                                   Vec3(20, 20, 20),
                                   Vec3(20, 40, 40),
                                   Vec3(20, 40, 40.00001f)};
    const float cellSize        = 1.0f / 62.0f;
    const Vec3  minimum(-32.0f / 62.0f);
    ASSERT_EQ(fixture.vertices.size(), 6u);
    for (uint32_t index = 0; index < 6; ++index)
    {
        fixture.vertices[index].pos = Vec4(minimum + gridPositions[index] * cellSize, 1);
    }
    const Vec3 edgeA = Vec3(fixture.vertices[4].pos - fixture.vertices[3].pos);
    const Vec3 edgeB = Vec3(fixture.vertices[5].pos - fixture.vertices[3].pos);
    EXPECT_GT(glm::length(glm::cross(edgeA, edgeB)), 0.0f);
    ASSERT_TRUE(fixture.scene->UpdateVertices(0, MakeVecView(fixture.vertices)));
    ASSERT_TRUE(fixture.scene->SetVoxelBounds(sg::AABB(Vec3(-0.5f), Vec3(0.5f))));
    ASSERT_TRUE(fixture.scene->Update());
    DistanceFieldSceneSettings settings = SmallFieldSettings();
    settings.cellsPerAxis               = 64;
    settings.atlasTexelsPerAxis         = 128;
    settings.cellBatchSize              = 256;
    settings.dispatchesPerFrame         = 4;
    DistanceFieldScene field(device, settings);
    ASSERT_TRUE(CompleteBuiltField(*device, *fixture.scene, field));
    ASSERT_EQ(field.GetState(), DistanceFieldScene::State::Ready) << field.GetFailureReason();
    EXPECT_EQ(field.GetFailureDetails().flags, 0u);
    EXPECT_NEAR(field.GetUniform().gridMinCellSize.w, cellSize, 1e-8f);
    const Vec3                         duplicate = Vec3(fixture.vertices[0].pos + fixture.vertices[1].pos) * 0.5f;
    const Vec3                         sliver    = minimum + Vec3(20, 30, 30) * cellSize;
    const HeapVector<BuiltFieldRay>    rays      = {{Vec4(duplicate - Vec3(0.05f, 0, 0), 0), Vec4(1, 0, 0, 0.1f)},
                                                    {Vec4(sliver - Vec3(0.05f, 0, 0), 0), Vec4(1, 0, 0, 0.1f)},
                                                    {Vec4(sliver + Vec3(0.05f, 0, 0), 0), Vec4(-1, 0, 0, 0.1f)}};
    const HeapVector<BuiltFieldResult> results   = TraceBuiltField(*device, field, rays);
    ASSERT_EQ(results.size(), rays.size());
    EXPECT_EQ(results[0].statistics.x, 0u); // Exact zero-area primitive creates no occluder.
    for (uint32_t index = 1; index < results.size(); ++index)
    {
        SCOPED_TRACE(index);
        EXPECT_EQ(results[index].statistics.x, 1u);
        EXPECT_NEAR(results[index].positionDistance.w, 0.05f, cellSize / 7.0f);
    }
}

TEST_P(DistanceFieldSceneIntegrationTest, DistanceFieldBuildPublicationAndSceneReplacement)
{
    RegisterDistanceFieldPrograms(device);
    BuiltFieldSceneFixture first;
    BuiltFieldSceneFixture second;
    ASSERT_TRUE(first.Init(device, false));
    ASSERT_TRUE(second.Init(device, false));
    ASSERT_NE(first.scene->GetIdentity(), second.scene->GetIdentity());
    DistanceFieldSceneSettings settings = SmallFieldSettings();
    settings.persistentBudgetBytes      = 2 * 1024 * 1024; // Native allocation: one output fits, two do not.
    DistanceFieldScene field(device, settings);
    ASSERT_TRUE(SubmitBuiltFieldFrame(*device, *first.scene, field));
    EXPECT_EQ(field.GetState(), DistanceFieldScene::State::Pending);
    RDGComputePassDesc unavailable;
    EXPECT_FALSE(field.BindTraceInputs(unavailable));
    const uint64_t retainedBefore = field.GetRetainedInputBytes();
    EXPECT_GT(retainedBefore, 0u);
    const uint64_t revision = first.scene->GetGeometryRevision();
    for (uint32_t edit = 0; edit < 4; ++edit)
    {
        sg::MaterialData material  = first.scene->GetMaterialsData()[0];
        material.baseColorFactor.x = 0.2f + 0.1f * edit;
        ASSERT_TRUE(first.scene->UpdateMaterial(0, material));
        ASSERT_TRUE(first.scene->Update());
        ASSERT_TRUE(SubmitBuiltFieldFrame(*device, *first.scene, field));
        EXPECT_EQ(first.scene->GetGeometryRevision(), revision);
        if (edit == 0)
        {
            EXPECT_EQ(field.GetState(), DistanceFieldScene::State::Pending);
            EXPECT_LT(field.GetRetainedInputBytes(), retainedBefore);
        }
    }
    ASSERT_TRUE(CompleteBuiltField(*device, *first.scene, field));
    ASSERT_EQ(field.GetState(), DistanceFieldScene::State::Ready);
    EXPECT_LE(field.GetPersistentBytes(), settings.persistentBudgetBytes);
    EXPECT_GT(2 * field.GetPersistentBytes(), settings.persistentBudgetBytes);
    const uint64_t originalGeneration = field.GetGeneration();
    EXPECT_EQ(originalGeneration, 1u); // RGB-only ring updates do not restart the snapshot.
    ASSERT_EQ(first.scene->GetGeometryRevision(), second.scene->GetGeometryRevision());
    ASSERT_TRUE(SubmitBuiltFieldFrame(*device, *second.scene, field));
    EXPECT_EQ(field.GetState(), DistanceFieldScene::State::Pending);
    EXPECT_EQ(field.GetGeneration(), 0u);
    // Transforms committed while a generation builds do not cancel it; the field is current only once a
    // generation of the newest transforms has published, and traces then see the final placement.
    for (uint32_t edit = 0; edit < 3; ++edit)
    {
        ASSERT_TRUE(second.scene->SetInstanceTransform(0, glm::translate(Mat4(1), Vec3(0.4f + 0.1f * edit, 0, 0))));
        ASSERT_TRUE(second.scene->Update());
        ASSERT_TRUE(SubmitBuiltFieldFrame(*device, *second.scene, field));
    }
    ASSERT_TRUE(CompleteBuiltField(*device, *second.scene, field));
    ASSERT_EQ(field.GetState(), DistanceFieldScene::State::Ready) << field.GetFailureReason();
    EXPECT_GT(field.GetGeneration(), originalGeneration);
    EXPECT_LE(field.GetPersistentBytes(), settings.persistentBudgetBytes);
    const HeapVector<BuiltFieldRay>    rays    = {{Vec4(0, 0, 0, 0), Vec4(1, 0, 0, 4)}, {Vec4(2, 0, 0, 0), Vec4(-1, 0, 0, 4)}};
    const HeapVector<BuiltFieldResult> results = TraceBuiltField(*device, field, rays);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].statistics.x, 1u);
    EXPECT_EQ(results[1].statistics.x, 1u);
    EXPECT_NEAR(results[0].positionDistance.w, 1.35f, field.GetUniform().gridMinCellSize.w * 0.3f);
    EXPECT_NEAR(results[1].positionDistance.w, 0.65f, field.GetUniform().gridMinCellSize.w * 0.3f);
}

TEST_P(DistanceFieldSceneIntegrationTest, DistanceFieldBuildConstantCoverageAndCapacityOverflow)
{
    RegisterDistanceFieldPrograms(device);
    BuiltFieldSceneFixture fixture;
    ASSERT_TRUE(fixture.Init(device, false, Vec3(1), true));
    DistanceFieldScene field(device, SmallFieldSettings());
    ASSERT_TRUE(CompleteBuiltField(*device, *fixture.scene, field));
    ASSERT_EQ(field.GetState(), DistanceFieldScene::State::Ready) << field.GetFailureReason();
    const HeapVector<BuiltFieldRay>    rays = {{Vec4(0, 0, 0, 0), Vec4(1, 0, 0, 4)}, {Vec4(2, 0, 0, 0), Vec4(-1, 0, 0, 4)}};
    const HeapVector<BuiltFieldResult> opaqueResults = TraceBuiltField(*device, field, rays);
    ASSERT_EQ(opaqueResults.size(), rays.size());
    EXPECT_EQ(opaqueResults[0].statistics.x, 1u);
    EXPECT_EQ(opaqueResults[1].statistics.x, 1u);
    for (asset::Vertex& vertex : fixture.vertices)
    {
        vertex.color.a = 0.0f;
    }
    ASSERT_TRUE(fixture.scene->UpdateVertices(0, MakeVecView(fixture.vertices)));
    ASSERT_TRUE(fixture.scene->Update());
    ASSERT_TRUE(SubmitBuiltFieldFrame(*device, *fixture.scene, field));
    ASSERT_TRUE(CompleteBuiltField(*device, *fixture.scene, field));
    ASSERT_EQ(field.GetState(), DistanceFieldScene::State::Ready) << field.GetFailureReason();
    const HeapVector<BuiltFieldResult> results = TraceBuiltField(*device, field, rays);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].statistics.x, 0u);
    EXPECT_EQ(results[1].statistics.x, 0u);

    BuiltFieldSceneFixture opaque;
    ASSERT_TRUE(opaque.Init(device, false));
    DistanceFieldSceneSettings tiny = SmallFieldSettings();
    tiny.atlasTexelsPerAxis         = 8;
    DistanceFieldScene overflow(device, tiny);
    ASSERT_TRUE(CompleteBuiltField(*device, *opaque.scene, overflow));
    EXPECT_EQ(overflow.GetState(), DistanceFieldScene::State::Failed);
    EXPECT_EQ(overflow.GetProcessedTriangleCount(), 0u); // Allocation failed before any emission completed.
    EXPECT_LE(overflow.GetProcessedTriangleCount(), opaque.scene->GetVoxelTriangleCount());
    RDGComputePassDesc blocked;
    EXPECT_FALSE(overflow.BindTraceInputs(blocked));
    // A scene-sized diagonal triangle spans many 4^3 regions. Exhausting its
    // reference budget must fail, even when every individual region job fits.
    BuiltFieldSceneFixture diagonal;
    ASSERT_TRUE(diagonal.Init(device, true));
    DistanceFieldSceneSettings references = SmallFieldSettings();
    references.cellsPerAxis               = 64;
    references.referenceCapacity          = 1;
    DistanceFieldScene referenceOverflow(device, references);
    ASSERT_TRUE(CompleteBuiltField(*device, *diagonal.scene, referenceOverflow));
    EXPECT_EQ(referenceOverflow.GetState(), DistanceFieldScene::State::Failed);
    EXPECT_EQ(referenceOverflow.GetGeneration(), 0u);
    EXPECT_EQ(referenceOverflow.GetProcessedTriangleCount(), 0u); // Preallocation retry exhausted its minimum quota.
    EXPECT_LE(referenceOverflow.GetProcessedTriangleCount(), diagonal.scene->GetVoxelTriangleCount());
    DistanceFieldSceneSettings budget = SmallFieldSettings();
    budget.scratchBudgetBytes         = 1;
    DistanceFieldScene rejected(device, budget);
    ASSERT_TRUE(SubmitBuiltFieldFrame(*device, *opaque.scene, rejected));
    EXPECT_EQ(rejected.GetState(), DistanceFieldScene::State::Failed);
    EXPECT_EQ(rejected.GetPersistentBytes(), 0u);
    EXPECT_EQ(rejected.GetScratchBytes(), 0u);
}

TEST_P(DistanceFieldSceneIntegrationTest, DistanceFieldBuildRetriesDeclinedGraphs)
{
    RegisterDistanceFieldPrograms(device);
    BuiltFieldSceneFixture fixture;
    ASSERT_TRUE(fixture.Init(device, false));
    DistanceFieldSceneSettings settings        = SmallFieldSettings();
    settings.throughputFrameBudgetMicroseconds = 100000.0;
    DistanceFieldScene field(device, settings);
    RenderGraph&       initial = *device->GetCurrentFrameRDG();
    ASSERT_TRUE(initial.Begin());
    EXPECT_EQ(field.BuildRenderGraph(*fixture.scene), DistanceFieldScene::State::Pending);
    ASSERT_TRUE(initial.End());
    field.OnRenderGraphExecuted(RenderFrameResult::eDeclined);
    EXPECT_EQ(field.GetState(), DistanceFieldScene::State::Pending);
    EXPECT_EQ(field.GetSubmittedGraphCount(), 0u);
    EXPECT_EQ(field.GetGeneration(), 0u);
    device->NextFrame();
    ASSERT_TRUE(SubmitBuiltFieldFrame(*device, *fixture.scene, field));
    ASSERT_TRUE(SubmitBuiltFieldFrame(*device, *fixture.scene, field));
    const uint64_t submitted = field.GetSubmittedGraphCount();
    EXPECT_EQ(submitted, 2u);
    RenderGraph& middle = *device->GetCurrentFrameRDG();
    ASSERT_TRUE(middle.Begin());
    EXPECT_EQ(field.BuildRenderGraph(*fixture.scene), DistanceFieldScene::State::Pending);
    ASSERT_TRUE(middle.End());
    field.OnRenderGraphExecuted(RenderFrameResult::eDeclined);
    EXPECT_EQ(field.GetState(), DistanceFieldScene::State::Pending);
    EXPECT_EQ(field.GetSubmittedGraphCount(), submitted);
    EXPECT_EQ(field.GetGeneration(), 0u);
    device->NextFrame();
    ASSERT_TRUE(CompleteBuiltField(*device, *fixture.scene, field));
    EXPECT_EQ(field.GetState(), DistanceFieldScene::State::Ready) << field.GetFailureReason();
    EXPECT_EQ(field.GetGeneration(), 1u);
    EXPECT_GT(field.GetSubmittedGraphCount(), submitted);
}

TEST_P(DistanceFieldSceneIntegrationTest, DistanceFieldRetriesDenseBatchBeforeAllocation)
{
    RegisterDistanceFieldPrograms(device);
    BuiltFieldSceneFixture fixture;
    ASSERT_TRUE(fixture.Init(device, false));
    DistanceFieldSceneSettings settings = SmallFieldSettings();
    settings.triangleBatchSize          = 2;
    settings.referencesPerCell          = 1;
    DistanceFieldScene field(device, settings);
    ASSERT_TRUE(CompleteBuiltField(*device, *fixture.scene, field));
    ASSERT_EQ(field.GetState(), DistanceFieldScene::State::Ready) << field.GetFailureReason();
    EXPECT_GT(field.GetBuildRetryCount(), 0u);
    // The accepted single-triangle batches filled their cells' only reference slot, so a doubled
    // batch would overflow again: the quota grows only with measured headroom.
    EXPECT_EQ(field.GetTriangleBatchQuota(), 1u);
    EXPECT_EQ(field.GetProcessedTriangleCount(), fixture.scene->GetVoxelTriangleCount());
    EXPECT_EQ(field.GetFailureDetails().flags, 0u);
    const HeapVector<BuiltFieldRay>    rays = {{Vec4(0, 0, 0, 0), Vec4(1, 0, 0, 2)}, {Vec4(1.5f, 0, 0, 0), Vec4(-1, 0, 0, 2)}};
    const HeapVector<BuiltFieldResult> results = TraceBuiltField(*device, field, rays);
    ASSERT_EQ(results.size(), rays.size());
    for (const BuiltFieldResult& result : results)
    {
        EXPECT_EQ(result.statistics.x, 1u);
        EXPECT_NEAR(result.positionDistance.w, 0.75f, field.GetUniform().gridMinCellSize.w * 0.3f);
    }
}

TEST_P(DistanceFieldSceneIntegrationTest, DistanceFieldThroughputPreservesDenseCellsInEveryBuild)
{
    RegisterDistanceFieldPrograms(device);
    HeapVector<Vec3> triangles;
    for (uint32_t triangle = 0; triangle < 2049; ++triangle)
    {
        const float x = triangle == 2048 ? 0.65f : 0.75f;
        triangles.push_back(Vec3(x, -0.6f, -0.6f));
        triangles.push_back(Vec3(x, 0.6f, -0.6f));
        triangles.push_back(Vec3(x, 0, 0.6f));
    }
    BuiltFieldSceneFixture fixture;
    ASSERT_TRUE(fixture.InitTriangles(device, triangles, sg::AABB(Vec3(-1), Vec3(1.5f))));
    // Capacities come from the scene and free memory. One batch puts all 2,049 references in the
    // dense cells, which the emit splits into three chunks; the nearest triangle is in the last.
    DistanceFieldSceneSettings settings        = SmallFieldSettings();
    settings.triangleBatchSize                 = 4096;
    settings.referencesPerCell                 = ZEN_FIELD_MAX_CELL_REFS;
    settings.deriveCapacities                  = true;
    settings.maxCyclesPerFrame                 = 16;
    settings.throughputFrameBudgetMicroseconds = 100000.0;
    DistanceFieldScene field(device, settings);
    ASSERT_TRUE(CompleteBuiltField(*device, *fixture.scene, field));
    ASSERT_EQ(field.GetState(), DistanceFieldScene::State::Ready) << field.GetFailureReason();
    EXPECT_EQ(field.GetBuildRetryCount(), 0u);
    EXPECT_EQ(field.GetProcessedTriangleCount(), 2049u);
    const HeapVector<BuiltFieldRay>    rays    = {{Vec4(0, 0, 0, 0), Vec4(1, 0, 0, 2)}};
    const HeapVector<BuiltFieldResult> initial = TraceBuiltField(*device, field, rays);
    ASSERT_EQ(initial.size(), 1u);
    EXPECT_EQ(initial[0].statistics.x, 1u);
    // The nearest triangle is beyond the old 1,024-reference limit in the source. Large
    // batches must retain it, and a later edit rebuilds without a retry.
    EXPECT_NEAR(initial[0].positionDistance.w, 0.65f, field.GetUniform().gridMinCellSize.w * 0.3f);
    ASSERT_TRUE(fixture.scene->SetInstanceTransform(0, glm::translate(Mat4(1), Vec3(0.05f, 0, 0))));
    ASSERT_TRUE(fixture.scene->Update());
    ASSERT_TRUE(CompleteBuiltField(*device, *fixture.scene, field));
    ASSERT_EQ(field.GetState(), DistanceFieldScene::State::Ready) << field.GetFailureReason();
    EXPECT_EQ(field.GetBuildRetryCount(), 0u);
    EXPECT_EQ(field.GetProcessedTriangleCount(), 2049u);
    const HeapVector<BuiltFieldResult> updated = TraceBuiltField(*device, field, rays);
    ASSERT_EQ(updated.size(), 1u);
    EXPECT_EQ(updated[0].statistics.x, 1u);
    EXPECT_NEAR(updated[0].positionDistance.w, 0.70f, field.GetUniform().gridMinCellSize.w * 0.3f);
}

TEST_P(DistanceFieldSceneIntegrationTest, DistanceFieldEmptySceneAndReplacement)
{
    RegisterDistanceFieldPrograms(device);
    BuiltFieldSceneFixture empty;
    BuiltFieldSceneFixture occupied;
    const asset::Vertex    dummyVertex{};
    const uint32_t         dummyIndex = 0;
    SceneData              data{};
    data.pScene    = &empty.source;
    data.pCamera   = &empty.camera;
    data.pVertices = &dummyVertex;
    data.pIndices  = &dummyIndex;
    empty.scene    = MakeUnique<RenderScene>(device, data);
    empty.scene->LoadSceneMaterials();
    empty.scene->PrepareBuffers();
    ASSERT_TRUE(empty.scene->SetVoxelBounds(sg::AABB(Vec3(-1), Vec3(1))));
    ASSERT_TRUE(empty.scene->Update());
    ASSERT_EQ(empty.scene->GetVoxelTriangleCount(), 0u);
    ASSERT_EQ(empty.scene->GetVoxelTriangleBuffer(), nullptr);
    ASSERT_TRUE(occupied.Init(device, false));
    ASSERT_NE(empty.scene->GetIdentity(), occupied.scene->GetIdentity());
    DistanceFieldScene field(device, SmallFieldSettings());
    ASSERT_TRUE(CompleteBuiltField(*device, *empty.scene, field));
    ASSERT_EQ(field.GetState(), DistanceFieldScene::State::Ready) << field.GetFailureReason();
    const uint64_t                     originalGeneration = field.GetGeneration();
    const HeapVector<BuiltFieldRay>    rays               = {{Vec4(-0.9f, 0, 0, 0), Vec4(1, 0, 0, 1.8f)},
                                                             {Vec4(0.9f, 0, 0, 0), Vec4(-1, 0, 0, 1.8f)}};
    const HeapVector<BuiltFieldResult> misses             = TraceBuiltField(*device, field, rays);
    ASSERT_EQ(misses.size(), rays.size());
    for (const BuiltFieldResult& result : misses)
    {
        EXPECT_EQ(result.statistics.x, 0u);
    }
    ASSERT_TRUE(SubmitBuiltFieldFrame(*device, *empty.scene, field));
    EXPECT_EQ(field.GetGeneration(), originalGeneration);
    const uint64_t previousRevision = empty.scene->GetGeometryRevision();
    ASSERT_TRUE(empty.scene->SetVoxelBounds(sg::AABB(Vec3(-2), Vec3(2))));
    ASSERT_TRUE(empty.scene->Update());
    EXPECT_GT(empty.scene->GetGeometryRevision(), previousRevision);
    // The previous generation stays published while the edit builds.
    ASSERT_TRUE(SubmitBuiltFieldFrame(*device, *empty.scene, field));
    EXPECT_EQ(field.GetState(), DistanceFieldScene::State::Ready);
    EXPECT_EQ(field.GetGeneration(), originalGeneration);
    EXPECT_FALSE(field.IsCurrent(*empty.scene));
    ASSERT_TRUE(CompleteBuiltField(*device, *empty.scene, field));
    ASSERT_EQ(field.GetState(), DistanceFieldScene::State::Ready) << field.GetFailureReason();
    EXPECT_GT(field.GetGeneration(), originalGeneration);
    const uint64_t emptyGeneration = field.GetGeneration();
    ASSERT_TRUE(SubmitBuiltFieldFrame(*device, *occupied.scene, field));
    EXPECT_EQ(field.GetState(), DistanceFieldScene::State::Pending);
    ASSERT_TRUE(CompleteBuiltField(*device, *occupied.scene, field));
    ASSERT_EQ(field.GetState(), DistanceFieldScene::State::Ready) << field.GetFailureReason();
    EXPECT_GT(field.GetGeneration(), emptyGeneration);
    const HeapVector<BuiltFieldResult> hits = TraceBuiltField(*device, field, rays);
    ASSERT_EQ(hits.size(), rays.size());
    for (const BuiltFieldResult& result : hits)
    {
        EXPECT_EQ(result.statistics.x, 1u);
    }
}

TEST_P(DistanceFieldSceneIntegrationTest, DistanceFieldStreamsMixedCoverageBeforeCompletingBatch)
{
    RegisterDistanceFieldPrograms(device);
    BuiltFieldSceneFixture fixture;
    ASSERT_TRUE(fixture.Init(device, false, Vec3(1), true));
    ASSERT_EQ(fixture.vertices.size(), 6u);
    // The first triangle is provably covered. The second has a covered lower
    // region and an open upper region, requiring many retained edge patches.
    fixture.vertices[4].color.a = 0.0f;
    fixture.vertices[5].color.a = 0.0f;
    ASSERT_TRUE(fixture.scene->UpdateVertices(0, MakeVecView(fixture.vertices)));
    ASSERT_TRUE(fixture.scene->Update());
    DistanceFieldSceneSettings settings = SmallFieldSettings();
    settings.triangleBatchSize          = 2;
    settings.referencesPerCell          = 32;
    settings.dispatchesPerFrame         = 4;
    DistanceFieldScene field(device, settings);
    ASSERT_TRUE(CompleteBuiltField(*device, *fixture.scene, field, 4096));
    ASSERT_EQ(field.GetState(), DistanceFieldScene::State::Ready) << field.GetFailureReason();
    EXPECT_EQ(field.GetProcessedTriangleCount(), fixture.scene->GetVoxelTriangleCount());
    EXPECT_EQ(field.GetTriangleBatchQuota(), settings.triangleBatchSize);
    EXPECT_EQ(field.GetFailureDetails().flags, 0u);
    // Both opaque and streamed covered patches survive, while the rejected
    // portion of the same original triangle remains an opening from both sides.
    const HeapVector<BuiltFieldRay> rays = {
        {Vec4(0, 0.4f, -0.4f, 0), Vec4(1, 0, 0, 2)},  {Vec4(1.5f, 0.4f, -0.4f, 0), Vec4(-1, 0, 0, 2)},
        {Vec4(0, -0.4f, -0.2f, 0), Vec4(1, 0, 0, 2)}, {Vec4(1.5f, -0.4f, -0.2f, 0), Vec4(-1, 0, 0, 2)},
        {Vec4(0, -0.4f, 0.4f, 0), Vec4(1, 0, 0, 2)},  {Vec4(1.5f, -0.4f, 0.4f, 0), Vec4(-1, 0, 0, 2)}};
    const HeapVector<BuiltFieldResult> results = TraceBuiltField(*device, field, rays);
    ASSERT_EQ(results.size(), rays.size());
    for (uint32_t index = 0; index < results.size(); ++index)
    {
        EXPECT_EQ(results[index].statistics.x, index < 4u ? 1u : 0u) << "Ray " << index;
        if (index < 4u)
        {
            EXPECT_NEAR(results[index].positionDistance.w, 0.75f, field.GetUniform().gridMinCellSize.w * 0.3f);
        }
    }
}

// Construction reads committed GPU geometry: shared mesh instances with rotation and nonuniform scale,
// transform and vertex edits, staged versus committed changes, enable/disable, class changes and
// colour-only material edits, which keep the published generation.
TEST_P(DistanceFieldSceneIntegrationTest, DistanceFieldBuildUsesCommittedSharedGeometry)
{
    RegisterDistanceFieldPrograms(device);
    sg::Scene                 source;
    HeapVector<asset::Vertex> vertices;
    HeapVector<uint32_t>      indices;
    UniquePtr<sg::Material>   material = MakeUnique<sg::Material>("shared field material");
    material->index                    = 0;
    UniquePtr<sg::Mesh> mesh           = MakeUnique<sg::Mesh>("shared field mesh");
    mesh->SetAABB(Vec3(-0.2f), Vec3(0.2f));
    AddFieldTestTriangle(source, *mesh, *material, vertices, indices, Vec3(-0.2f, -0.2f, 0), Vec3(0.2f, -0.2f, 0),
                         Vec3(0, 0.2f, 0));
    const Mat4 transforms[] = {glm::translate(Mat4(1), Vec3(-0.75f, 0, 0)) * glm::rotate(Mat4(1), 0.5f, Vec3(0, 1, 0))
                                   * glm::scale(Mat4(1), Vec3(0.8f, 0.7f, 1)),
                               glm::translate(Mat4(1), Vec3(0.75f, 0, 0)) * glm::scale(Mat4(1), Vec3(0.8f, 0.7f, 2))};
    HeapVector<UniquePtr<sg::Node>> nodes;
    for (uint32_t instance = 0; instance < 2; ++instance)
    {
        UniquePtr<sg::Node> node = MakeUnique<sg::Node>(instance, "shared field instance");
        node->SetData(instance, transforms[instance]);
        node->AddComponent(mesh.Get());
        source.AddRenderableNode(node.Get());
        nodes.push_back(std::move(node));
    }
    source.SetNodes(std::move(nodes));
    source.AddComponent(std::move(mesh));
    source.AddComponent(std::move(material));
    source.GetAABB() = sg::AABB(Vec3(-0.5f), Vec3(0.5f));
    sg::Camera camera(Vec3(0, 0, -5), Vec3(0), 1);
    SceneData  data{};
    data.pScene      = &source;
    data.pVertices   = vertices.data();
    data.numVertices = static_cast<uint32_t>(vertices.size());
    data.pIndices    = indices.data();
    data.numIndices  = static_cast<uint32_t>(indices.size());
    data.pCamera     = &camera;
    asset::PrepareScenePlacement(source, asset::ScenePlacement::PreserveAuthored);
    RenderScene scene(device, data);
    scene.LoadSceneMaterials();
    scene.PrepareBuffers();
    ASSERT_TRUE(scene.Update());

    HeapVector<BuiltFieldRay> rays;
    for (uint32_t instance = 0; instance < 2; ++instance)
    {
        rays.push_back({Vec4(Vec3(transforms[instance] * Vec4(0, -0.05f, -1, 1)), 0),
                        Vec4(glm::normalize(Vec3(transforms[instance] * Vec4(0, 0, 1, 0))), 8)});
    }
    // Hit distance per frame and instance ray; negative is a covered miss.
    const float        distances[][2] = {{1, 2},         {1.125f, 2},     {1.25f, 2.25f},  {-1, 2.25f},     {1.25f, 2.25f},
                                         {1.25f, 2.25f}, {1.375f, 2.25f}, {1.375f, 2.25f}, {1.375f, 2.25f}, {-1, -1}};
    DistanceFieldScene field(device, SmallFieldSettings());
    uint64_t           generation = 0;
    for (uint32_t frame = 0; frame < std::size(distances); ++frame)
    {
        SCOPED_TRACE(frame);
        if (frame == 1 || frame == 5)
        {
            const float offset = frame == 1 ? 0.125f : 0.25f;
            ASSERT_TRUE(scene.SetInstanceTransform(0, transforms[0] * glm::translate(Mat4(1), Vec3(0, 0, offset))));
        }
        if (frame == 2)
        {
            for (asset::Vertex& vertex : vertices)
            {
                vertex.pos.z += 0.125f;
            }
            ASSERT_TRUE(scene.UpdateVertices(0, MakeVecView(vertices)));
        }
        if (frame == 3 || frame == 4 || frame == 9)
        {
            ASSERT_TRUE(scene.SetInstanceEnabled(0, frame == 4));
            if (frame == 9)
            {
                ASSERT_TRUE(scene.SetInstanceEnabled(1, false));
            }
        }
        if (frame == 7)
        {
            const uint64_t   geometryRevision = scene.GetGeometryRevision();
            sg::MaterialData changed          = scene.GetMaterialsData()[0];
            changed.baseColorFactor.r         = 0.25f;
            ASSERT_TRUE(scene.UpdateMaterial(0, changed));
            ASSERT_TRUE(scene.Update());
            EXPECT_EQ(scene.GetGeometryRevision(), geometryRevision);
        }
        if (frame == 8)
        {
            ASSERT_TRUE(scene.SetInstanceClass(1, GI_DYNAMIC));
        }
        // An uncommitted transform must not reach the field or invalidate its published generation.
        if (frame != 5)
        {
            ASSERT_TRUE(scene.Update());
        }
        // A published field notices a committed edit on its next frame, then rebuilds.
        ASSERT_TRUE(SubmitBuiltFieldFrame(*device, scene, field));
        ASSERT_TRUE(CompleteBuiltField(*device, scene, field));
        ASSERT_EQ(field.GetState(), DistanceFieldScene::State::Ready) << field.GetFailureReason();
        if (frame == 1)
        {
            EXPECT_EQ(field.GetProcessedTriangleCount(), 1u) << "The unaffected instance must not be rebuilt";
        }
        if (frame == 5 || frame == 7)
        {
            EXPECT_EQ(field.GetGeneration(), generation);
        }
        generation                                 = field.GetGeneration();
        const HeapVector<BuiltFieldResult> results = TraceBuiltField(*device, field, rays, true);
        ASSERT_EQ(results.size(), rays.size());
        // The march accepts a hit within twice the tolerance of a surface; interpolation adds up
        // to half a sample spacing.
        const float spacing = field.GetUniform().gridMinCellSize.w / 7.0f;
        for (uint32_t ray = 0; ray < 2; ++ray)
        {
            SCOPED_TRACE(ray);
            const float expected = distances[frame][ray];
            EXPECT_EQ(results[ray].statistics.x, expected < 0 ? 0u : 1u);
            if (expected >= 0)
            {
                EXPECT_NEAR(results[ray].positionDistance.w, expected, spacing);
            }
        }
    }
    field.Destroy();
    scene.Destroy();
}

// A 1.2 cm slit lies inside the scene field's hit band, so the scene field closes it. A local volume
// around the slit resolves it: the composite trace passes through to the wall behind. Moving the
// volume drops it back to the scene field until its rebuild is published.
TEST_P(DistanceFieldSceneIntegrationTest, DistanceFieldLocalVolumeOpensSlit)
{
    RegisterDistanceFieldPrograms(device);
    const float            gap       = 0.006f;
    const HeapVector<Vec3> triangles = {
        // Slit walls at y = +-gap, 0.2 m long along the ray, and the wall at x = 1.5 behind them.
        Vec3(-0.1f, gap, -1), Vec3(0.1f, gap, -1),   Vec3(0.1f, gap, 1),  Vec3(-0.1f, gap, -1), Vec3(0.1f, gap, 1),
        Vec3(-0.1f, gap, 1),  Vec3(-0.1f, -gap, -1), Vec3(0.1f, -gap, 1), Vec3(0.1f, -gap, -1), Vec3(-0.1f, -gap, -1),
        Vec3(-0.1f, -gap, 1), Vec3(0.1f, -gap, 1),   Vec3(1.5f, -1, -1),  Vec3(1.5f, 1, -1),    Vec3(1.5f, 1, 1),
        Vec3(1.5f, -1, -1),   Vec3(1.5f, 1, 1),      Vec3(1.5f, -1, 1)};
    BuiltFieldSceneFixture fixture;
    ASSERT_TRUE(fixture.InitTriangles(device, triangles, sg::AABB(Vec3(-0.1f, -1, -1), Vec3(1.5f, 1, 1))));
    DistanceFieldScene field(device, SmallFieldSettings());
    ASSERT_TRUE(CompleteBuiltField(*device, *fixture.scene, field));
    ASSERT_EQ(field.GetState(), DistanceFieldScene::State::Ready) << field.GetFailureReason();
    const float    sceneBrick  = field.GetUniform().gridMinCellSize.w;
    const uint32_t sceneBricks = field.GetAllocatedBrickCount();
    ASSERT_GT(2.0f * field.GetUniform().tracing.x * sceneBrick, gap);
    const HeapVector<BuiltFieldRay> rays    = {{Vec4(-0.35f, 0, 0, 0), Vec4(1, 0, 0, 3)}};
    HeapVector<BuiltFieldResult>    results = TraceBuiltField(*device, field, rays, true);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].statistics.x, 1u);
    EXPECT_LT(results[0].positionDistance.x, 0.0f) << "the scene field closes the slit";

    HeapVector<AABB> cores = {AABB(Vec3(-0.15f), Vec3(0.15f))};
    ASSERT_TRUE(field.SetLocalVolumes(cores));
    for (uint32_t frame = 0; frame < 512 && field.GetReadyLocalVolumeCount() == 0; ++frame)
    {
        ASSERT_TRUE(SubmitBuiltFieldFrame(*device, *fixture.scene, field));
    }
    ASSERT_EQ(field.GetReadyLocalVolumeCount(), 1u);
    ASSERT_TRUE(SubmitBuiltFieldFrame(*device, *fixture.scene, field));
    EXPECT_EQ(field.GetAllocatedBrickCount(), sceneBricks);
    EXPECT_EQ(field.GetProcessedTriangleCount(), fixture.scene->GetVoxelTriangleCount());
    const DistanceFieldLocalRecord& record = field.GetLocalsUniform().volumes[0];
    EXPECT_EQ(field.GetLocalsUniform().header.x, 1u);
    EXPECT_NE(record.slot.z, 0u);
    EXPECT_LT(record.gridMinCellSize.w * 4.0f, sceneBrick);
    EXPECT_LE(record.authorityMinimum.x, -0.15f);
    results = TraceBuiltField(*device, field, rays, true);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].statistics.x, 1u);
    EXPECT_NEAR(results[0].positionDistance.x, 1.5f, sceneBrick * 0.3f) << "the local volume opens the slit";
    // The scene field alone still closes it.
    results = TraceBuiltField(*device, field, rays);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_LT(results[0].positionDistance.x, 0.0f);

    // A moved volume traces the scene field until its rebuild is published.
    const uint32_t revision = field.GetLocalRevision();
    cores[0]                = AABB(Vec3(-0.15f, -0.15f, -0.2f), Vec3(0.15f, 0.15f, 0.2f));
    ASSERT_TRUE(field.SetLocalVolumes(cores));
    EXPECT_NE(field.GetLocalRevision(), revision);
    EXPECT_EQ(field.GetReadyLocalVolumeCount(), 0u);
    ASSERT_TRUE(SubmitBuiltFieldFrame(*device, *fixture.scene, field));
    EXPECT_EQ(field.GetLocalsUniform().volumes[0].slot.z, 0u);
    for (uint32_t frame = 0; frame < 512 && field.GetReadyLocalVolumeCount() == 0; ++frame)
    {
        ASSERT_TRUE(SubmitBuiltFieldFrame(*device, *fixture.scene, field));
    }
    ASSERT_EQ(field.GetReadyLocalVolumeCount(), 1u);
    ASSERT_TRUE(SubmitBuiltFieldFrame(*device, *fixture.scene, field));
    results = TraceBuiltField(*device, field, rays, true);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_NEAR(results[0].positionDistance.x, 1.5f, sceneBrick * 0.3f);
    // An oversized set is rejected and keeps the published volume.
    EXPECT_FALSE(field.SetLocalVolumes(HeapVector<AABB>(ZEN_FIELD_LOCAL_VOLUMES + 1, cores[0])));
    EXPECT_EQ(field.GetReadyLocalVolumeCount(), 1u);
}

TEST_P(DistanceFieldSceneIntegrationTest, DistanceFieldRefreshReclaimsQueuedLocalScratch)
{
    RegisterDistanceFieldPrograms(device);
    BuiltFieldSceneFixture fixture;
    ASSERT_TRUE(fixture.Init(device, false));
    DistanceFieldSceneSettings settings = SmallFieldSettings();
    settings.localCellsPerAxis          = 8;
    settings.localAtlasTexelsPerAxis    = 32;
    {
        DistanceFieldScene measured(device, settings);
        ASSERT_TRUE(SubmitBuiltFieldFrame(*device, *fixture.scene, measured));
        // One snapshot and workspace fit; retaining a second snapshot does not.
        settings.scratchBudgetBytes = measured.GetScratchBytes();
    }
    device->FlushRHIThread();
    device->WaitForIdle();
    DistanceFieldScene field(device, settings);
    ASSERT_TRUE(field.SetLocalVolumes({AABB(Vec3(-0.1f), Vec3(0.1f)), AABB(Vec3(0.2f), Vec3(0.4f))}));
    ASSERT_TRUE(CompleteBuiltField(*device, *fixture.scene, field));
    ASSERT_TRUE(field.LocalWorkRemains());
    ASSERT_TRUE(fixture.scene->SetInstanceTransform(0, glm::translate(Mat4(1), Vec3(0.1f, 0, 0))));
    ASSERT_TRUE(fixture.scene->Update());
    for (uint32_t frame = 0; frame < 1024 && (!field.IsCurrent(*fixture.scene) || field.LocalWorkRemains()); ++frame)
    {
        ASSERT_TRUE(SubmitBuiltFieldFrame(*device, *fixture.scene, field));
        ASSERT_LE(field.GetScratchBytes(), settings.scratchBudgetBytes);
    }
    EXPECT_TRUE(field.IsCurrent(*fixture.scene));
    EXPECT_EQ(field.GetReadyLocalVolumeCount(), 2u);
}

TEST_P(DistanceFieldSceneIntegrationTest, DistanceFieldLocalPoolRespectsPersistentBudget)
{
    RegisterDistanceFieldPrograms(device);
    BuiltFieldSceneFixture fixture;
    ASSERT_TRUE(fixture.Init(device, false));
    DistanceFieldSceneSettings settings = SmallFieldSettings();
    settings.persistentBudgetBytes      = 2 * 1024 * 1024;
    DistanceFieldScene field(device, settings);
    ASSERT_TRUE(field.SetLocalVolumes({AABB(Vec3(-0.1f), Vec3(0.1f))}));
    ASSERT_TRUE(CompleteBuiltField(*device, *fixture.scene, field));
    // The default detail atlas cannot fit, but the global field remains usable and refinement ends.
    ASSERT_TRUE(SubmitBuiltFieldFrame(*device, *fixture.scene, field));
    EXPECT_FALSE(field.LocalWorkRemains());
    EXPECT_EQ(field.GetReadyLocalVolumeCount(), 0u);
    EXPECT_LE(field.GetPersistentBytes(), settings.persistentBudgetBytes);
    RDGComputePassDesc trace;
    EXPECT_TRUE(field.BindCompositeInputs(trace));
}

TEST_P(DistanceFieldSceneIntegrationTest, DistanceFieldDynamicUpdatesKeepStaticGeneration)
{
    RegisterDistanceFieldPrograms(device);
    BuiltFieldSceneFixture fixture;
    ASSERT_TRUE(fixture.Init(device, false));
    ASSERT_TRUE(fixture.scene->SetInstanceClass(0, GI_DYNAMIC));
    ASSERT_TRUE(fixture.scene->Update());
    DistanceFieldSceneSettings settings = SmallFieldSettings();
    settings.localCellsPerAxis          = 16;
    settings.localAtlasTexelsPerAxis    = 96;
    DistanceFieldScene field(device, settings);
    ASSERT_TRUE(CompleteBuiltField(*device, *fixture.scene, field));
    ASSERT_EQ(field.GetState(), DistanceFieldScene::State::Ready) << field.GetFailureReason();
    const uint64_t                  generation = field.GetGeneration();
    const uint32_t                  revision   = field.GetLocalRevision();
    const HeapVector<BuiltFieldRay> rays       = {{Vec4(0, 0, 0, 0), Vec4(1, 0, 0, 8)}};
    for (uint32_t frame = 0; frame < 24; ++frame)
    {
        const float offset = 0.05f + 0.002f * float(frame);
        ASSERT_TRUE(fixture.scene->SetInstanceTransform(0, glm::translate(Mat4(1), Vec3(offset, 0, 0))));
        ASSERT_TRUE(fixture.scene->Update());
        ASSERT_TRUE(SubmitBuiltFieldFrame(*device, *fixture.scene, field));
        EXPECT_EQ(field.GetGeneration(), generation);
        if (frame == 0)
        {
            const HeapVector<BuiltFieldResult> stale = TraceBuiltField(*device, field, rays, true);
            ASSERT_EQ(stale.size(), 1u);
            EXPECT_EQ(stale[0].statistics.x, 2u);
        }
    }
    ASSERT_TRUE(CompleteBuiltField(*device, *fixture.scene, field));
    EXPECT_EQ(field.GetGeneration(), generation);
    EXPECT_GT(field.GetLocalRevision(), revision);
    const HeapVector<BuiltFieldResult> moved = TraceBuiltField(*device, field, rays, true);
    ASSERT_EQ(moved.size(), 1u);
    EXPECT_EQ(moved[0].statistics.x, 1u);
    EXPECT_NEAR(moved[0].positionDistance.w, 0.846f, 0.04f);

    // Dynamic coverage can expand independently of the static field and the fixed lighting cache.
    ASSERT_TRUE(fixture.scene->SetInstanceTransform(0, glm::translate(Mat4(1), Vec3(6, 0, 0))));
    ASSERT_TRUE(fixture.scene->Update());
    ASSERT_TRUE(SubmitBuiltFieldFrame(*device, *fixture.scene, field));
    EXPECT_FALSE(field.IsCurrent(*fixture.scene));
    ASSERT_TRUE(CompleteBuiltField(*device, *fixture.scene, field));
    EXPECT_EQ(field.GetGeneration(), generation);
    const HeapVector<BuiltFieldResult> expanded = TraceBuiltField(*device, field, rays, true);
    ASSERT_EQ(expanded.size(), 1u);
    EXPECT_EQ(expanded[0].statistics.x, 1u);
    EXPECT_NEAR(expanded[0].positionDistance.w, 6.75f, 0.04f);
    EXPECT_LE(field.GetPersistentBytes(), settings.persistentBudgetBytes);
}

TEST_P(DistanceFieldSceneIntegrationTest, DistanceFieldMandatoryDynamicBudgetFailureIsExplicit)
{
    RegisterDistanceFieldPrograms(device);
    BuiltFieldSceneFixture fixture;
    ASSERT_TRUE(fixture.Init(device, false));
    ASSERT_TRUE(fixture.scene->SetInstanceClass(0, GI_DYNAMIC));
    ASSERT_TRUE(fixture.scene->Update());
    DistanceFieldSceneSettings settings = SmallFieldSettings();
    settings.persistentBudgetBytes      = 2 * 1024 * 1024;
    DistanceFieldScene field(device, settings);
    ASSERT_TRUE(CompleteBuiltField(*device, *fixture.scene, field));
    EXPECT_EQ(field.GetState(), DistanceFieldScene::State::Failed);
    EXPECT_FALSE(field.IsCurrent(*fixture.scene));
    EXPECT_LE(field.GetPersistentBytes(), settings.persistentBudgetBytes);
}

TEST_P(DistanceFieldSceneIntegrationTest, DistanceFieldSkyCacheAndLocalLightSeeExternalBlocker)
{
    RegisterDistanceFieldPrograms(device);
    ShaderProgramManager::GetInstance().StoreProgram(
        ZEN_NEW() ComputeFileSP(device, "FieldSkyConsumerSP", "VoxelGI/sky_irradiance_field.comp.spv"));
    ShaderProgramManager::GetInstance().StoreProgram(
        ZEN_NEW() ComputeFileSP(device, "FieldLightingCheckSP", "VoxelGI/Calibration/distance_field_lighting_check.comp.spv"));
    BuiltFieldSceneFixture fixture;
    ASSERT_TRUE(fixture.Init(device, false));
    DistanceFieldScene field(device, SmallFieldSettings());
    ASSERT_TRUE(CompleteBuiltField(*device, *fixture.scene, field));
    RHISampler*          sampler = device->CreateSampler({});
    RHITextureCreateInfo info;
    info.type  = RHITextureType::e3D;
    info.width = info.height = info.depth = 1;
    info.format                           = DataFormat::eR16G16B16A16SFloat;
    info.usageFlags.SetFlags(RHITextureUsageFlagBits::eSampled, RHITextureUsageFlagBits::eStorage,
                             RHITextureUsageFlagBits::eTransferDst);
    for (uint32_t texture = 0; texture < 5; ++texture)
    {
        if (texture == 3)
        {
            info.format = DataFormat::eR32UInt;
        }
        if (texture == 4)
        {
            info.format      = DataFormat::eR16G16B16A16SFloat;
            info.type        = RHITextureType::eCube;
            info.arrayLayers = 6;
            info.usageFlags  = 0;
            info.usageFlags.SetFlags(RHITextureUsageFlagBits::eSampled, RHITextureUsageFlagBits::eTransferDst);
        }
        textures.push_back(device->CreateTexture(info));
        ASSERT_NE(textures.back(), nullptr);
    }
    const uint32_t                         noOwner = UINT32_MAX;
    HeapVector<float>                      zeroDistribution(ZEN_ENVIRONMENT_IMPORTANCE_ROWS + 1, 0.0f);
    const BitField<RHIBufferUsageFlagBits> storage(RHIBufferUsageFlagBits::eStorageBuffer);
    RHIBuffer*                             distribution =
        Buffer(static_cast<uint32_t>(zeroDistribution.size() * sizeof(float)), storage, zeroDistribution.data());
    RHIBuffer* owner =
        Buffer(sizeof(noOwner), BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferSrcBuffer), &noOwner);
    BitField<RHIBufferUsageFlagBits> outputUsage;
    outputUsage.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer);
    RHIBuffer* output   = Buffer(3 * sizeof(Vec4), outputUsage, nullptr, RHIBufferAllocateType::eGPU);
    RHIBuffer* readback = Buffer(3 * sizeof(Vec4), BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferDstBuffer),
                                 nullptr, RHIBufferAllocateType::eCPURead);
    SceneUniformData sceneData;
    sceneData.environment = Vec4(1, 0, 1, 0);
    const VoxelGIUniformData gi{Vec4(-0.25f, -0.25f, -0.25f, 0.5f), Vec4(1, 2, 1, 1), Vec4(0), Vec4(0)};
    for (uint32_t scenario = 0; scenario < 5; ++scenario)
    {
        SCOPED_TRACE(scenario);
        if (scenario == 1 || scenario == 2 || scenario == 4)
        {
            ASSERT_TRUE(fixture.scene->SetInstanceEnabled(0, scenario == 2));
        }
        if (scenario == 3)
        {
            ASSERT_TRUE(fixture.scene->SetInstanceTransform(0, glm::translate(Mat4(1), Vec3(6, 0, 0))));
        }
        if (scenario != 0)
        {
            ASSERT_TRUE(fixture.scene->Update());
            ASSERT_TRUE(SubmitBuiltFieldFrame(*device, *fixture.scene, field));
            EXPECT_FALSE(field.IsCurrent(*fixture.scene));
            ASSERT_TRUE(CompleteBuiltField(*device, *fixture.scene, field));
        }
        ASSERT_EQ(field.GetState(), DistanceFieldScene::State::Ready) << field.GetFailureReason();
        EXPECT_EQ(fixture.scene->GetVoxelSceneBounds().GetMin(), Vec3(-0.25f));
        RenderGraph& graph = *device->GetCurrentFrameRDG();
        ASSERT_TRUE(graph.Begin());
        graph.GetResourceManager()->ImportHostWrittenBuffer(distribution);
        graph.GetResourceManager()->ImportHostWrittenBuffer(owner);
        graph.AddTransferPass("SkyFixtureAlbedo").ClearTexture(textures[0], Color(0.5f, 0.5f, 0.5f, 1));
        graph.AddTransferPass("SkyFixtureNormal").ClearTexture(textures[1], Color(1, 0.5f, 0.5f, 1));
        graph.AddTransferPass("SkyFixtureEnvironment").ClearTexture(textures[4], Color(1));
        RHIBufferTextureCopyRegion region;
        region.textureSize = Vec3i(1);
        region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
        graph.AddTransferPass("SkyFixtureOwner").CopyBufferToTexture(owner, textures[3], region);
        RDGComputePassDesc sky;
        sky.SetShaderProgramName("FieldSkyConsumerSP");
        sky.BindValue("uGISettings", gi);
        sky.BindValue("uSceneData", sceneData);
        ASSERT_TRUE(field.BindCompositeInputs(sky));
        sky.BindStorageBuffer("VertexBuffer", fixture.scene->GetVertexBuffer());
        sky.BindStorageBuffer("IndexBuffer", fixture.scene->GetIndexBuffer());
        sky.BindStorageBuffer("NodeBuffer", fixture.scene->GetNodesDataSSBO());
        sky.BindStorageBuffer("TriangleRecords", fixture.scene->GetVoxelTriangleBuffer());
        for (const char* name : {"EnvironmentColumns", "EnvironmentRows", "EnvironmentLuminance", "EnvironmentTiles",
                                 "EnvironmentSources", "EnvironmentResidualColumns", "EnvironmentResidualRows"})
        {
            sky.BindStorageBuffer(name, distribution);
        }
        sky.BindSampledTexture("voxelAlbedo", sampler, textures[0]->GetDefaultView());
        sky.BindSampledTexture("voxelNormal", sampler, textures[1]->GetDefaultView());
        sky.BindSampledTexture("coneEnvironmentMap", sampler, textures[4]->GetDefaultView());
        sky.BindStorageImage("voxelOwner", textures[3]->GetDefaultView());
        sky.BindStorageImage("skyIrradiance", textures[2]->GetDefaultView(), RDGContentGuarantee::eFullWrite);
        graph.AddComputePass(std::move(sky)).RecordPassCommands([](RDGPassCmdEncoder& encoder) { encoder.Dispatch(1, 1, 1); });
        RDGComputePassDesc check;
        check.SetShaderProgramName("FieldLightingCheckSP");
        check.BindValue("uGISettings", gi);
        check.BindValue("uSceneData", sceneData);
        check.BindValue("uFieldLightingCheck", Vec4(scenario >= 3 ? 10.0f : 2.0f, 0, 0, 0));
        ASSERT_TRUE(field.BindCompositeInputs(check));
        check.BindSampledTexture("testedSky", sampler, textures[2]->GetDefaultView());
        check.BindStorageBuffer("FieldLightingResults", output, RDGContentGuarantee::eFullWrite);
        graph.AddComputePass(std::move(check)).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
            encoder.Dispatch(1, 1, 1);
        });
        graph.AddTransferPass("ReadFieldLighting").CopyBuffer(output, readback, {0, 0, 3 * sizeof(Vec4)}).NeverCull();
        ASSERT_TRUE(graph.End()) << graph.GetResult().message;
        ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
        device->FlushRHIThread();
        device->WaitForIdle();
        const Vec4* result = reinterpret_cast<const Vec4*>(readback->Map());
        ASSERT_NE(result, nullptr);
        const bool blocked = scenario == 0 || scenario == 2 || scenario == 3;
        EXPECT_EQ(result[1].x, blocked ? 0.0f : 1.0f);
        EXPECT_EQ(result[1].y, blocked ? 1.0f : 0.0f);
        EXPECT_EQ(result[1].z, 1.0f);
        EXPECT_EQ(result[1].w, -1.0f) << "External hits must not address the radiance cache";
        EXPECT_EQ(result[2].x, 1.0f) << "An unobstructed finite segment can leave scene bounds";
        EXPECT_EQ(result[0].w, 2.0f);
        if (scenario == 0 || scenario == 2)
        {
            EXPECT_LT(result[0].x, 2.8f) << "The external blocker must suppress sky irradiance";
        }
        else if (!blocked)
        {
            EXPECT_NEAR(result[0].x, 3.14159265f, 0.005f);
        }
        readback->Unmap();
    }
}
