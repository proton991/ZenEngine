// Included by the native Vulkan ray-query suite so every case runs in all four submission modes.
struct BrickOracleRay
{
    Vec4  start;
    Vec4  direction;
    float distance;
};

void AddBrickOracleRay(HeapVector<BrickOracleRay>& rays,
                       const Vec3&                 origin,
                       const Vec3&                 direction,
                       float                       distance,
                       float                       maximum = 8.0f)
{
    rays.push_back({Vec4(origin, maximum), Vec4(glm::normalize(direction), 0), distance});
}

void AddBrickOracleTriangle(sg::Scene&                 source,
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
        MakeUnique<sg::SubMesh>("brick oracle triangle", static_cast<uint32_t>(indices.size()), 3, 3);
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

bool TraceBrickOracleFrame(RenderDevice&          device,
                           RenderScene&           scene,
                           SoftwareSceneRayQuery& triangles,
                           BrickScene&            bricks,
                           RHIBuffer*             rayBuffer,
                           RHIBuffer*             output,
                           RHIBuffer*             readback,
                           uint32_t               rayCount,
                           uint32_t               resolution)
{
    RenderGraph& graph     = *device.GetCurrentFrameRDG();
    bool         succeeded = triangles.Prepare(scene) && graph.Begin();
    if (succeeded)
    {
        succeeded = bricks.BuildRenderGraph(scene, triangles, Vec3(-2), 4.0f, resolution);
        EXPECT_TRUE(succeeded);
        if (succeeded)
        {
            graph.GetResourceManager()->ImportHostWrittenBuffer(rayBuffer);
            RDGComputePassDesc pass;
            pass.SetShaderProgramName("BrickTraceCheckSP");
            pass.SetPassTag("BrickTraceOracle");
            pass.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
            pass.BindStorageBuffer("Rays", rayBuffer);
            pass.BindStorageBuffer("Results", output, RDGContentGuarantee::eFullWrite);
            const Vec4 gi[] = {Vec4(-2, -2, -2, 4.0f / 64.0f), Vec4(64), Vec4(0), Vec4(0), Vec4(0), Vec4(1, 0, 0, 0)};
            pass.BindValue("uGISettings", gi, sizeof(gi));
            pass.BindValue("uSceneData", scene.GetSceneUniformData(), sizeof(SceneUniformData));
            bricks.BindTraceInputs(pass);
            const uint32_t groups = (rayCount + 63u) / 64u;
            graph.AddComputePass(std::move(pass)).RecordPassCommands([groups](RDGPassCmdEncoder& encoder) {
                encoder.Dispatch(groups, 1, 1);
            });
            graph.AddTransferPass("ReadBrickOracle").CopyBuffer(output, readback, {0, 0, rayCount * 2u * sizeof(Vec4)});
        }
        succeeded = graph.End() && succeeded;
        EXPECT_TRUE(succeeded) << graph.GetResult().message;
        if (succeeded)
        {
            succeeded = device.ExecuteRenderGraph(graph);
        }
        const RenderFrameResult result = succeeded ? RenderFrameResult::eSubmitted : RenderFrameResult::eFailed;
        bricks.OnRenderGraphExecuted(result);
        triangles.OnRenderGraphExecuted(result);
        device.WaitForIdle();
    }
    return succeeded;
}

TEST_P(RayQueryIntegrationTest, BrickBuildAndTraceMatchesAnalyticSurfaces)
{
    const char* programs[][2] = {{"BrickClearSP", "brick_clear"},
                                 {"BrickMarkSP", "brick_mark"},
                                 {"BrickFinalizeSP", "brick_finalize"},
                                 {"BrickMarkLargeSP", "brick_mark_large"},
                                 {"BrickAllocateSP", "brick_allocate"},
                                 {"BrickRasterizeSP", "brick_rasterize"},
                                 {"BrickRasterizeLargeSP", "brick_rasterize_large"},
                                 {"BrickPruneSP", "brick_prune"},
                                 {"BrickDistanceXSP", "brick_distance_x"},
                                 {"BrickDistanceYSP", "brick_distance_y"},
                                 {"BrickDistanceZSP", "brick_distance_z"},
                                 {"BrickTraceCheckSP", "Calibration/brick_trace_check"}};
    for (const char* const(&program)[2] : programs)
    {
        ShaderProgramManager::GetInstance().StoreProgram(
            ZEN_NEW() ComputeFileSP(device, program[0], std::string("VoxelGI/") + program[1] + ".comp.spv"));
    }

    sg::Scene                 source;
    HeapVector<asset::Vertex> vertices;
    HeapVector<uint32_t>      indices;
    UniquePtr<sg::Material>   opaque = MakeUnique<sg::Material>("brick opaque");
    UniquePtr<sg::Material>   masked = MakeUnique<sg::Material>("brick masked");
    opaque->index                    = 0;
    masked->index                    = 1;
    masked->data.surfaceProperties.y = 1.0f;
    UniquePtr<sg::Mesh> mesh         = MakeUnique<sg::Mesh>("brick oracle");
    mesh->SetAABB(Vec3(-2), Vec3(2));
    // A closed box whose faces cross many cells exercises the large-triangle dispatch.
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        for (float side : {-0.6f, 0.6f})
        {
            Vec3           a(0), b(0), c(0), d(0);
            const uint32_t u = (axis + 1u) % 3u, v = (axis + 2u) % 3u;
            a[axis] = b[axis] = c[axis] = d[axis] = side;
            a[u] = a[v] = b[v] = d[u] = -0.6f;
            b[u] = c[u] = c[v] = d[v] = 0.6f;
            AddBrickOracleTriangle(source, *mesh, *opaque, vertices, indices, a, b, c);
            AddBrickOracleTriangle(source, *mesh, *opaque, vertices, indices, a, c, d);
        }
    }
    // A thin, isolated small triangle uses the regular per-triangle rasterization path.
    AddBrickOracleTriangle(source, *mesh, *opaque, vertices, indices, Vec3(0.9f, 0.9f, 1.1f), Vec3(1.05f, 0.9f, 1.1f),
                           Vec3(0.9f, 1.05f, 1.1f));
    // Vertex alpha splits this large triangle into accepted and rejected coverage.
    AddBrickOracleTriangle(source, *mesh, *masked, vertices, indices, Vec3(-1.7f, -0.7f, 0.7f), Vec3(-0.9f, -0.7f, 0.7f),
                           Vec3(-1.7f, 0.7f, 0.7f), Vec3(1, 0, 0));
    UniquePtr<sg::Node> node = MakeUnique<sg::Node>(0, "brick oracle instance");
    node->SetData(0, Mat4(1));
    node->AddComponent(mesh.Get());
    source.AddRenderableNode(node.Get());
    HeapVector<UniquePtr<sg::Node>> nodes;
    nodes.push_back(std::move(node));
    source.SetNodes(std::move(nodes));
    source.AddComponent(std::move(mesh));
    source.AddComponent(std::move(opaque));
    source.AddComponent(std::move(masked));
    source.GetAABB() = sg::AABB(Vec3(-0.5f), Vec3(0.5f)); // Identity scene normalization.
    sg::Camera camera(Vec3(0, 0, -5), Vec3(0), 1);
    SceneData  data{};
    data.pScene      = &source;
    data.pVertices   = vertices.data();
    data.numVertices = static_cast<uint32_t>(vertices.size());
    data.pIndices    = indices.data();
    data.numIndices  = static_cast<uint32_t>(indices.size());
    data.pCamera     = &camera;
    asset::PrepareScenePlacement(source, asset::ScenePlacement::CenterAndNormalize);
    RenderScene scene(device, data);
    scene.LoadSceneMaterials();
    scene.PrepareBuffers();
    ASSERT_TRUE(scene.Update());

    HeapVector<BrickOracleRay> cases;
    // Analytic face distances, including rays from outside the grid, negative travel, shared
    // triangle diagonals, oblique travel, finite maxima, empty interiors and starts inside a face.
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        for (float sign : {-1.0f, 1.0f})
        {
            const uint32_t u = (axis + 1u) % 3u, v = (axis + 2u) % 3u;
            Vec3           normal(0);
            normal[axis] = sign;
            for (uint32_t row = 0; row < 7; ++row)
            {
                for (uint32_t column = 0; column < 7; ++column)
                {
                    Vec3 point       = normal * 0.6f;
                    point[u]         = -0.36f + 0.12f * static_cast<float>(column);
                    point[v]         = -0.36f + 0.12f * static_cast<float>(row);
                    const Vec3 start = point + normal * 1.9f;
                    AddBrickOracleRay(cases, start, -normal, 1.9f);
                    AddBrickOracleRay(cases, start, -normal, -1.0f, 1.8f);
                    Vec3 oblique = normal;
                    oblique[u]   = 0.15f;
                    oblique[v]   = -0.1f;
                    oblique      = glm::normalize(oblique);
                    AddBrickOracleRay(cases, point + oblique, -oblique, 1.0f);
                    // Starting inside the face's voxels skips them: leaving outward escapes, and
                    // crossing the hollow interior hits the opposite face.
                    AddBrickOracleRay(cases, point, normal, -1.0f);
                    AddBrickOracleRay(cases, point, -normal, 1.2f);
                }
            }
            AddBrickOracleRay(cases, Vec3(0), normal, 0.6f);
            // Starts exactly on a cell boundary in empty space, travelling in its negative direction.
            AddBrickOracleRay(cases, normal * 1.0f, -normal, 0.4f);
            Vec3 outside = normal * 2.5f;
            outside[u]   = 0.8f;
            AddBrickOracleRay(cases, outside, -normal, -1.0f);
        }
    }
    // Equal DDA crossing times must not skip the box corners in either travel direction.
    for (uint32_t corner = 0; corner < 8; ++corner)
    {
        const Vec3 signs((corner & 1u) != 0u ? 1.0f : -1.0f, (corner & 2u) != 0u ? 1.0f : -1.0f,
                         (corner & 4u) != 0u ? 1.0f : -1.0f);
        AddBrickOracleRay(cases, signs * 1.5f, -signs, std::sqrt(3.0f) * 0.9f);
    }
    AddBrickOracleRay(cases, Vec3(0.94f, 0.94f, 0.5f), Vec3(0, 0, 1), 0.6f);
    AddBrickOracleRay(cases, Vec3(1.1f, 0.94f, 0.5f), Vec3(0, 0, 1), -1.0f);
    const uint32_t maskedHitRay = static_cast<uint32_t>(cases.size());
    AddBrickOracleRay(cases, Vec3(-1.58f, -0.49f, 0), Vec3(0, 0, 1), 0.7f);
    AddBrickOracleRay(cases, Vec3(-1.18f, -0.49f, 0), Vec3(0, 0, 1), -1.0f);
    // Parallel rays outside the slab and rays pointing away from the entire grid must miss.
    AddBrickOracleRay(cases, Vec3(2.0f, 0, 0), Vec3(0, 1, 0), -1.0f);
    AddBrickOracleRay(cases, Vec3(0, 0, 2.5f), Vec3(0, 0, 1), -1.0f);

    HeapVector<Vec4> rays;
    for (const BrickOracleRay& ray : cases)
    {
        rays.push_back(ray.start);
        rays.push_back(ray.direction);
    }
    const uint32_t bytes = static_cast<uint32_t>(rays.size() * sizeof(Vec4));
    RHIBuffer* rayBuffer = Buffer(bytes, BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eStorageBuffer), rays.data());
    BitField<RHIBufferUsageFlagBits> outputUsage;
    outputUsage.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer);
    RHIBuffer* output   = Buffer(bytes, outputUsage, nullptr, RHIBufferAllocateType::eGPU);
    RHIBuffer* readback = Buffer(bytes, BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferDstBuffer), nullptr,
                                 RHIBufferAllocateType::eCPURead);
    ASSERT_NE(rayBuffer, nullptr);
    ASSERT_NE(output, nullptr);
    ASSERT_NE(readback, nullptr);
    SoftwareSceneRayQuery triangles(device);
    BrickScene            bricks(device);
    // Reuse, resize, material coverage and empty-scene rebuilds preserve the trace contract.
    const uint32_t resolutions[] = {64, 64, 128, 128, 128};
    bool           framesReady   = true;
    for (uint32_t frame = 0; frame < std::size(resolutions) && framesReady; ++frame)
    {
        SCOPED_TRACE(frame);
        const uint32_t resolution = resolutions[frame];
        const bool     empty      = frame == 4;
        if (frame == 3)
        {
            sg::MaterialData transparentMask  = scene.GetMaterialsData()[1];
            transparentMask.baseColorFactor.a = 0.0f;
            ASSERT_TRUE(scene.UpdateMaterial(1, transparentMask));
            ASSERT_TRUE(scene.Update());
        }
        if (empty)
        {
            ASSERT_TRUE(scene.SetInstanceEnabled(0, false));
            ASSERT_TRUE(scene.Update());
        }
        const bool traced = TraceBrickOracleFrame(*device, scene, triangles, bricks, rayBuffer, output, readback,
                                                  static_cast<uint32_t>(cases.size()), resolution);
        framesReady       = traced;
        EXPECT_TRUE(traced);
        if (traced)
        {
            CheckQueryQueue(*device->GetCurrentFrameRDG());
            const BrickSceneStatistics statistics = bricks.ReadStatistics();
            EXPECT_FALSE(statistics.overflow);
            EXPECT_EQ(statistics.largeTriangles, empty ? 0u : frame == 3 ? 12u : 13u);
            EXPECT_EQ(statistics.allocatedBricks == 0u, empty);
            const Vec4* values    = reinterpret_cast<const Vec4*>(readback->Map());
            const float tolerance = 2.0f * 4.0f / static_cast<float>(resolution * 8u);
            for (uint32_t ray = 0; ray < cases.size(); ++ray)
            {
                SCOPED_TRACE(ray);
                const BrickOracleRay& expected = cases[ray];
                const Vec4            position = values[2u * ray], normal = values[2u * ray + 1u];
                const bool            hit = !empty && expected.distance >= 0 && !(frame == 3 && ray == maskedHitRay);
                EXPECT_EQ(position.w, hit ? 1.0f : 0.0f);
                if (hit && position.w > 0.5f)
                {
                    EXPECT_NEAR(normal.w, expected.distance, tolerance);
                    const Vec3 expectedPosition = Vec3(expected.start) + Vec3(expected.direction) * expected.distance;
                    EXPECT_LE(glm::length(Vec3(position) - expectedPosition), tolerance);
                    EXPECT_FLOAT_EQ(glm::length(Vec3(normal)), 1.0f);
                    EXPECT_LT(glm::dot(Vec3(normal), Vec3(expected.direction)), 0.0f);
                }
            }
            readback->Unmap();
        }
    }
    bricks.Destroy();
    triangles.Destroy();
    scene.Destroy();
}
