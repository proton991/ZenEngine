// Optional scene-scale construction experiment. The input is a prepared .zenasset
// package; no source importer, CPU BVH, or hardware ray query participates.
struct FieldAssetSceneResources
{
    RenderScene& scene;
    ~FieldAssetSceneResources()
    {
        scene.Destroy();
    }
};

bool FieldAssetOption(const char* name, uint32_t& value)
{
    const char* text  = std::getenv(name);
    bool        valid = true;
    if (text != nullptr)
    {
        char*               end    = nullptr;
        const unsigned long parsed = std::strtoul(text, &end, 10);
        valid                      = end != text && *end == '\0' && parsed <= UINT32_MAX;
        if (valid)
        {
            value = static_cast<uint32_t>(parsed);
        }
    }
    return valid;
}

std::string FieldAssetTriangleEvidence(const asset::LoadedScene&        loaded,
                                       const RenderScene&               scene,
                                       const DistanceFieldSceneFailure& failure)
{
    std::string result;
    uint64_t    first = 0;
    for (const sg::Node* node : loaded.scene->GetRenderableNodes())
    {
        for (const sg::SubMesh* mesh : node->GetComponent<sg::Mesh>()->GetSubMeshes())
        {
            const uint32_t count = mesh->topology == sg::MeshTopology::Triangles ? mesh->GetIndexCount() / 3 : 0;
            if (failure.triangle >= first && failure.triangle < first + count)
            {
                const uint32_t index = mesh->GetFirstIndex() + 3 * static_cast<uint32_t>(failure.triangle - first);
                result               = fmt::format("node={} material={} first_index={}", node->GetRenderableIndex(),
                                                   mesh->GetMaterial()->index, index);
                for (uint32_t corner = 0; corner < 3; ++corner)
                {
                    const Vec3 local(scene.GetVertices()[scene.GetIndices()[index + corner]].pos);
                    const Vec3 world(node->GetData().modelMatrix * Vec4(local, 1));
                    const Vec3 field  = (world - Vec3(failure.uniform.gridMinCellSize)) / failure.uniform.gridMinCellSize.w;
                    result           += fmt::format(" p{}=({:.9g},{:.9g},{:.9g})", corner, field.x, field.y, field.z);
                }
            }
            first += count;
        }
    }
    return result;
}

TEST_P(DistanceFieldSceneIntegrationTest, DISABLED_DistanceFieldAssetConstruction)
{
    const char* path = std::getenv("ZEN_FIELD_ASSET_PATH");
    ASSERT_NE(path, nullptr) << "Set ZEN_FIELD_ASSET_PATH to a prepared scene package";
    const char*         limitText  = std::getenv("ZEN_FIELD_ASSET_MAX_FRAMES");
    const unsigned long frameLimit = limitText != nullptr ? std::strtoul(limitText, nullptr, 10) : 200000;
    ASSERT_GT(frameLimit, 0ul);
    ASSERT_LE(frameLimit, 1000000ul);
    asset::SceneLoadResult loaded = asset::LoadScene(path);
    ASSERT_TRUE(loaded.scene) << loaded.error;
    asset::PrepareScenePlacement(*loaded.scene->scene, asset::ScenePlacement::CenterAndNormalize);
    sg::Camera camera(Vec3(0, 0, -2), Vec3(0), 1);
    SceneData  data{};
    data.pScene      = loaded.scene->scene.Get();
    data.pCamera     = &camera;
    data.pVertices   = loaded.scene->vertices.data();
    data.numVertices = static_cast<uint32_t>(loaded.scene->vertices.size());
    data.pIndices    = loaded.scene->indices.data();
    data.numIndices  = static_cast<uint32_t>(loaded.scene->indices.size());
    ShaderProgramManager::GetInstance().BuildShaderPrograms(device);
    device->InitializeRendererServer();
    RenderScene              scene(device, data);
    FieldAssetSceneResources resources{scene};
    ASSERT_TRUE(scene.Init());
    ASSERT_TRUE(scene.Update());
    DistanceFieldSceneSettings settings;
    const char*                atlasText = std::getenv("ZEN_FIELD_ASSET_ATLAS_SIDE");
    const unsigned long        atlasSide = atlasText != nullptr ? std::strtoul(atlasText, nullptr, 10) : 256;
    ASSERT_GE(atlasSide, 8ul);
    ASSERT_LE(atlasSide, 512ul);
    ASSERT_EQ(atlasSide % 8, 0ul);
    settings.atlasTexelsPerAxis = static_cast<uint32_t>(atlasSide);
    settings.cellBatchSize      = 1024;
    settings.jobsPerDispatch    = 4096;
    settings.dispatchesPerFrame = 16;
    ASSERT_TRUE(FieldAssetOption("ZEN_FIELD_ASSET_TRIANGLE_BATCH", settings.triangleBatchSize));
    ASSERT_TRUE(FieldAssetOption("ZEN_FIELD_ASSET_CELL_BATCH", settings.cellBatchSize));
    ASSERT_TRUE(FieldAssetOption("ZEN_FIELD_ASSET_JOBS_PER_DISPATCH", settings.jobsPerDispatch));
    ASSERT_TRUE(FieldAssetOption("ZEN_FIELD_ASSET_DISPATCHES", settings.dispatchesPerFrame));
    ASSERT_TRUE(FieldAssetOption("ZEN_FIELD_ASSET_CELL_REFERENCES", settings.referencesPerCell));
    DistanceFieldScene   field(device, settings);
    FieldAssetGPUProfile profile;
    ASSERT_TRUE(profile.Init(*device, std::getenv("ZEN_FIELD_ASSET_PROFILE_PREFIX")));
    const std::chrono::steady_clock::time_point start              = std::chrono::steady_clock::now();
    uint64_t                                    peakPersistent     = 0;
    uint64_t                                    peakScratch        = 0;
    uint64_t                                    peakRetainedInputs = 0;
    uint32_t                                    frames             = 0;
    bool                                        valid              = true;
    while (valid && field.GetState() == DistanceFieldScene::State::Pending && frames < frameLimit)
    {
        profile.Begin(frames);
        RenderGraph& graph = *device->GetCurrentFrameRDG();
        valid              = graph.Begin();
        if (valid)
        {
            field.BuildRenderGraph(scene);
            peakPersistent     = std::max(peakPersistent, field.GetPersistentBytes());
            peakScratch        = std::max(peakScratch, field.GetScratchBytes());
            peakRetainedInputs = std::max(peakRetainedInputs, field.GetRetainedInputBytes());
            valid              = graph.End();
            if (valid)
            {
                valid = device->ExecuteRenderGraph(graph);
            }
            EXPECT_TRUE(valid) << graph.GetResult().message;
            field.OnRenderGraphExecuted(valid ? RenderFrameResult::eSubmitted : RenderFrameResult::eFailed);
        }
        profile.End(valid);
        ++frames;
        // Ordinary frame-slot retirement advances the asynchronous builder. No
        // synchronous counter readback or device-idle wait occurs in this loop.
        device->NextFrame();
        profile.Collect();
    }
    const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    profile.Finish();
    RecordProperty("profile_available_frames", std::to_string(profile.availableFrames));
    RecordProperty("profile_unavailable_frames", std::to_string(profile.unavailableFrames));
    RecordProperty("profile_available_passes", std::to_string(profile.availablePasses));
    RecordProperty("profile_unavailable_passes", std::to_string(profile.unavailablePasses));
    RecordProperty("profile_maximum_frame_gpu_us", std::to_string(profile.maximumFrameUs));
    RecordProperty("profile_maximum_pass_gpu_us", std::to_string(profile.maximumPassUs));
    RecordProperty("asset_path", std::string(path));
    RecordProperty("validation_enabled", RHIOptions::GetInstance().ValidationEnabled() ? "true" : "false");
    RecordProperty("gpu_markers_enabled", RHIOptions::GetInstance().GPUProfilerMarkers() ? "true" : "false");
    RecordProperty("vertices", std::to_string(data.numVertices));
    RecordProperty("triangles", std::to_string(scene.GetVoxelTriangleCount()));
    RecordProperty("atlas_texels_per_axis", std::to_string(settings.atlasTexelsPerAxis));
    RecordProperty("triangle_batch", std::to_string(settings.triangleBatchSize));
    RecordProperty("cell_batch", std::to_string(settings.cellBatchSize));
    RecordProperty("jobs_per_dispatch", std::to_string(settings.jobsPerDispatch));
    RecordProperty("dispatches_per_frame", std::to_string(settings.dispatchesPerFrame));
    RecordProperty("references_per_cell", std::to_string(settings.referencesPerCell));
    RecordProperty("allocated_or_attempted_bricks", std::to_string(field.GetAllocatedBrickCount()));
    RecordProperty("field_snapshot_bytes", std::to_string(field.GetSnapshotBytes()));
    RecordProperty("peak_field_persistent_bytes", std::to_string(peakPersistent));
    RecordProperty("peak_field_scratch_bytes", std::to_string(peakScratch));
    RecordProperty("peak_shared_retained_input_bytes", std::to_string(peakRetainedInputs));
    RecordProperty("submitted_construction_graphs", std::to_string(field.GetSubmittedGraphCount()));
    RecordProperty("build_batch_retries", std::to_string(field.GetBuildRetryCount()));
    RecordProperty("latest_triangle_batch_quota", std::to_string(field.GetTriangleBatchQuota()));
    RecordProperty("processed_triangles", std::to_string(field.GetProcessedTriangleCount()));
    RecordProperty("preparation_frames", std::to_string(frames));
    RecordProperty("preparation_elapsed_ms", std::to_string(elapsed));
    RecordProperty("failure_reason", std::string(field.GetFailureReason()));
    const DistanceFieldSceneFailure& failure = field.GetFailureDetails();
    RecordProperty("failure_flags", std::to_string(failure.flags));
    RecordProperty("failure_site", std::to_string(failure.site));
    RecordProperty("failure_triangle", std::to_string(failure.triangle));
    RecordProperty("failure_cell", std::to_string(failure.cell));
    RecordProperty("failure_sample", std::to_string(failure.sample));
    if (failure.site != 0)
    {
        RecordProperty("failure_triangle_grid_positions_cpu", FieldAssetTriangleEvidence(*loaded.scene, scene, failure));
        const Vec4 grid = failure.uniform.gridMinCellSize;
        RecordProperty("failure_grid", fmt::format("{:.9g},{:.9g},{:.9g},{:.9g} side={}", grid.x, grid.y, grid.z, grid.w,
                                                   failure.uniform.dimensions.x));
    }
    EXPECT_TRUE(valid);
    EXPECT_EQ(field.GetState(), DistanceFieldScene::State::Ready) << field.GetFailureReason();
    EXPECT_LE(peakPersistent, settings.persistentBudgetBytes);
    EXPECT_LE(peakScratch, settings.scratchBudgetBytes);
    field.Destroy();
}
