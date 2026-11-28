// Included inside the editor fixture namespace. These checks use cooked packages
// and production traversal in both compute and fragment shader invocations.
constexpr uint32_t kSpatialRaySide       = 32;
constexpr uint32_t kSpatialRayPanelCount = 6;
constexpr uint32_t kSpatialRayCount      = kSpatialRaySide * kSpatialRaySide * kSpatialRayPanelCount * 4;
constexpr uint32_t kSpatialRayWidth      = 128;
constexpr uint32_t kSpatialRayHeight     = kSpatialRayCount / kSpatialRayWidth;


HeapVector<Vec4> MakeSpatialAlphaRays(const rc::RenderScene& scene)
{
    HeapVector<Vec4> rays;
    rays.reserve(kSpatialRayCount * 2);
    for (uint32_t panel = 0; panel < kSpatialRayPanelCount; ++panel)
    {
        const Mat4& matrix = scene.GetInstanceTransform(panel);
        const Vec3  axis   = Vec3(matrix * Vec4(0, 0, 1, 0));
        const float scale  = glm::length(axis);
        const Vec3  normal = axis / scale;
        for (uint32_t mode = 0; mode < 4; ++mode)
        {
            for (uint32_t y = 0; y < kSpatialRaySide; ++y)
            {
                for (uint32_t x = 0; x < kSpatialRaySide; ++x)
                {
                    const Vec2  uv        = (Vec2(x, y) + Vec2(0.37f, 0.61f)) / float(kSpatialRaySide);
                    const Vec3  point     = Vec3(matrix * Vec4(-0.45f + uv.x * 0.9f, -0.45f + uv.y * 0.9f, 0, 1));
                    const Vec3  origin    = point + normal * (mode == 2 ? 0.125f * scale : -scale);
                    const Vec3  direction = mode == 2 ? -normal : normal;
                    const float maximum   = scale * (mode == 0 ? 2.0f : mode == 2 ? 1.0f : 1.1f);
                    rays.push_back(Vec4(origin, maximum));
                    rays.push_back(Vec4(direction, mode == 3 ? 0.0f : 1.0f));
                }
            }
        }
    }
    return rays;
}

class SpatialRayProbe
{
public:
    explicit SpatialRayProbe(rc::RenderDevice& device) : m_device(device), m_hardwareQueries(&device) {}

    ~SpatialRayProbe()
    {
        m_device.WaitForIdle();
        m_hardwareQueries.Destroy();
        m_device.DestroyBuffer(m_rays);
        m_device.DestroyBuffer(m_results);
        m_device.DestroyBuffer(m_readback);
    }

    void Initialize(const HeapVector<Vec4>& rays)
    {
        m_rays = m_device.CreateStorageBuffer(static_cast<uint32_t>(rays.size() * sizeof(Vec4)),
                                              reinterpret_cast<const uint8_t*>(rays.data()), "spatial_alpha_rays");
        RHIBufferCreateInfo info;
        info.size         = kSpatialRayCount * sizeof(Vec4);
        info.allocateType = RHIBufferAllocateType::eGPU;
        info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer);
        m_results         = m_device.CreateBuffer(info);
        info.allocateType = RHIBufferAllocateType::eCPURead;
        info.usageFlags.Clear();
        info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);
        m_readback = m_device.CreateBuffer(info);
        ASSERT_NE(m_rays, nullptr);
        ASSERT_NE(m_results, nullptr);
        ASSERT_NE(m_readback, nullptr);
    }

    void Trace(rc::RenderScene& scene, bool fragment, HeapVector<Vec4>& values)
    {
        rc::RenderGraph& graph = *m_device.GetCurrentFrameRDG();
        ASSERT_TRUE(graph.Begin());
        ASSERT_TRUE(m_hardwareQueries.BuildRenderGraph(scene)) << m_hardwareQueries.GetReason();
        if (fragment)
        {
            rc::RDGGraphicsPassDesc pass;
            pass.SetShaderProgramName("SpatialAlphaHardwareFragmentSP");
            BindInputs(pass, scene);
            RHIGfxPipelineStates state;
            state.primitiveType = RHIDrawPrimitiveType::eTriangleList;
            state.colorBlendState.AddAttachments(1);
            state.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewport);
            pass.SetPipelineStates(state);
            pass.AddColorOutput(DataFormat::eR8G8B8A8UNORM, kSpatialRayWidth, kSpatialRayHeight, "spatial_ray_coverage");
            pass.SetRenderArea(0, 0, kSpatialRayWidth, kSpatialRayHeight);
            graph.AddGraphicsPass(std::move(pass)).RecordPassCommands(Draw);
        }
        else
        {
            rc::RDGComputePassDesc pass;
            pass.SetShaderProgramName("SpatialAlphaHardwareComputeSP");
            BindInputs(pass, scene);
            graph.AddComputePass(std::move(pass)).RecordPassCommands(Dispatch);
        }
        graph.AddTransferPass("spatial_alpha_readback")
            .CopyBuffer(m_results, m_readback, {0, 0, kSpatialRayCount * sizeof(Vec4)});
        ASSERT_TRUE(graph.End()) << graph.GetResult().message;
        const bool                  succeeded = m_device.ExecuteRenderGraph(graph);
        const rc::RenderFrameResult result    = succeeded ? rc::RenderFrameResult::eSubmitted : rc::RenderFrameResult::eFailed;
        m_hardwareQueries.OnRenderGraphExecuted(result);
        ASSERT_TRUE(succeeded) << graph.GetResult().message;
        m_device.FlushRHIThread();
        m_device.WaitForIdle();
        const uint8_t* mapped = m_readback->Map();
        ASSERT_NE(mapped, nullptr);
        values.resize(kSpatialRayCount);
        std::memcpy(values.data(), mapped, kSpatialRayCount * sizeof(Vec4));
        m_readback->Unmap();
        graph.Reset();
    }

private:
    void BindInputs(rc::RDGPassDescBase& pass, rc::RenderScene& scene)
    {
        pass.BindStorageBuffer("Rays", m_rays);
        pass.BindStorageBuffer("Results", m_results, rc::RDGContentGuarantee::eFullWrite);
        const Vec4 gi[] = {Vec4(0, 0, 0, 1), Vec4(64), Vec4(0), Vec4(0), Vec4(0), Vec4(1, 0, 0, 0)};
        pass.BindValue("uGISettings", gi, sizeof(gi));
        pass.BindValue("uSceneData", scene.GetSceneUniformData(), sizeof(rc::SceneUniformData));
        m_hardwareQueries.BindInputs(pass, scene);
    }

    static void Dispatch(rc::RDGPassCmdEncoder& encoder)
    {
        encoder.Dispatch(kSpatialRayCount / 64, 1, 1);
    }
    static void Draw(rc::RDGPassCmdEncoder& encoder)
    {
        encoder.SetViewport(0, 0, kSpatialRayWidth, kSpatialRayHeight);
        encoder.SetScissor(0, 0, kSpatialRayWidth, kSpatialRayHeight);
        encoder.Draw(3, 1);
    }

    rc::RenderDevice& m_device;
    rc::SceneRayQuery m_hardwareQueries;
    RHIBuffer*        m_rays{nullptr};
    RHIBuffer*        m_results{nullptr};
    RHIBuffer*        m_readback{nullptr};
};

void CheckSpatialAlphaCoverage(const HeapVector<Vec4>& values, bool opaque)
{
    ASSERT_EQ(values.size(), kSpatialRayCount);
    for (uint32_t panel = 0; panel < kSpatialRayPanelCount; ++panel)
    {
        for (uint32_t mode = 0; mode < 4; ++mode)
        {
            uint32_t hits = 0, fronts = 0, rears = 0;
            for (uint32_t sample = 0; sample < kSpatialRaySide * kSpatialRaySide; ++sample)
            {
                const Vec4 value = values[(panel * 4 + mode) * kSpatialRaySide * kSpatialRaySide + sample];
                for (uint32_t channel = 0; channel < 4; ++channel)
                {
                    ASSERT_TRUE(std::isfinite(value[channel]));
                }
                hits   += value.x == 1 ? 1u : 0u;
                fronts += value.x == 1 && value.w == float(panel + 1) ? 1u : 0u;
                rears  += value.x == 1 && value.w == float(panel + 7) ? 1u : 0u;
            }
            SCOPED_TRACE("panel " + std::to_string(panel) + " mode " + std::to_string(mode));
            if (opaque)
            {
                EXPECT_EQ(hits, kSpatialRaySide * kSpatialRaySide);
                if (mode != 3)
                {
                    EXPECT_EQ(fronts, hits);
                }
            }
            else if (mode == 0)
            {
                EXPECT_EQ(hits, kSpatialRaySide * kSpatialRaySide);
                EXPECT_GT(fronts, 100u);
                EXPECT_GT(rears, 100u);
            }
            else
            {
                EXPECT_GT(hits, 100u);
                EXPECT_LT(hits, kSpatialRaySide * kSpatialRaySide - 100u);
                if (mode != 3)
                {
                    EXPECT_EQ(fronts, hits);
                }
            }
        }
    }
}

void SaveSpatialRayCapture(const std::filesystem::path& package, const std::string& suffix, const HeapVector<Vec4>& values)
{
    const char* directory = std::getenv("ZEN_EDITOR_SPATIAL_RAY_DIR");
    if (directory != nullptr)
    {
        const std::filesystem::path path = std::filesystem::path(directory) / (package.stem().string() + suffix + ".bin");
        std::ofstream               output(path, std::ios::binary);
        ASSERT_TRUE(output.is_open());
        output.write(reinterpret_cast<const char*>(values.data()), static_cast<std::streamsize>(values.size() * sizeof(Vec4)));
        ASSERT_TRUE(output.good());
    }
}

void CaptureSpatialAlphaQueries(rc::RenderDevice&             device,
                                EditorController&             editor,
                                const std::filesystem::path&  package,
                                HeapVector<HeapVector<Vec4>>& captures)
{
    ASSERT_TRUE(editor.Load(package.string())) << editor.GetError();
    EXPECT_FALSE(editor.CanRecook());
    rc::RenderScene* scene = const_cast<rc::RenderScene*>(editor.GetViewport().GetRenderScene());
    ASSERT_NE(scene, nullptr);
    // Seven authored materials plus the runtime fallback material.
    ASSERT_EQ(scene->GetMaterialsData().size(), 8u);
    SpatialRayProbe        probe(device);
    const HeapVector<Vec4> rays = MakeSpatialAlphaRays(*scene);
    ASSERT_NO_FATAL_FAILURE(probe.Initialize(rays));
    for (int32_t variant : {-1, 0})
    {
        ASSERT_TRUE(scene->SetMaterialVariant(variant));
        ASSERT_TRUE(scene->Update());
        for (bool fragment : {false, true})
        {
            captures.emplace_back();
            ASSERT_NO_FATAL_FAILURE(probe.Trace(*scene, fragment, captures.back()));
            ASSERT_NO_FATAL_FAILURE(CheckSpatialAlphaCoverage(captures.back(), false));
            ASSERT_NO_FATAL_FAILURE(SaveSpatialRayCapture(
                package, "-v" + std::to_string(variant) + (fragment ? "-fragment" : "-compute"), captures.back()));
        }
        const HeapVector<Vec4>& compute  = captures[captures.size() - 2];
        const HeapVector<Vec4>& fragment = captures.back();
        for (uint32_t ray = 0; ray < kSpatialRayCount; ++ray)
        {
            ASSERT_EQ(compute[ray].x, fragment[ray].x) << ray;
            ASSERT_EQ(compute[ray].z, fragment[ray].z) << ray;
            ASSERT_EQ(compute[ray].w, fragment[ray].w) << ray;
            ASSERT_NEAR(compute[ray].y, fragment[ray].y, 1e-5f) << ray;
        }
    }
    uint32_t variantCoverageChanges = 0;
    for (uint32_t ray = 0; ray < kSpatialRayCount; ++ray)
    {
        variantCoverageChanges +=
            captures[0][ray].x != captures[2][ray].x || captures[0][ray].w != captures[2][ray].w ? 1u : 0u;
    }
    EXPECT_GT(variantCoverageChanges, 100u);
    // An opaque control must close the holes; otherwise comparisons could pass
    // while texture alpha or the front surfaces were never used.
    for (uint32_t material = 0; material < 6; ++material)
    {
        sg::MaterialData data    = scene->GetMaterialsData()[material];
        data.surfaceProperties.y = 0;
        ASSERT_TRUE(scene->UpdateMaterial(material, data));
    }
    ASSERT_TRUE(scene->Update());
    for (bool fragment : {false, true})
    {
        HeapVector<Vec4> control;
        ASSERT_NO_FATAL_FAILURE(probe.Trace(*scene, fragment, control));
        ASSERT_NO_FATAL_FAILURE(CheckSpatialAlphaCoverage(control, true));
        ASSERT_NO_FATAL_FAILURE(SaveSpatialRayCapture(package, fragment ? "-opaque-fragment" : "-opaque-compute", control));
    }
}

void CompareCookedSpatialAlpha(RHIExecutionMode execution)
{
    EditorRenderingScope      scope(execution, true);
    EditorRenderingValidation validation;
    if (!scope.device->GetGPUInfo().rayQuery.IsUsable())
    {
        GTEST_SKIP() << "Hardware ray queries are unavailable";
    }
    ASSERT_TRUE(scope.editor.Init());
    const RHIGPUInfo& gpu = scope.device->GetGPUInfo();
    testing::Test::RecordProperty("gpu", gpu.deviceName.data());
    testing::Test::RecordProperty("driver_version", std::to_string(gpu.driverVersionRaw));
    testing::Test::RecordProperty("subtexel_precision_bits", gpu.subTexelPrecisionBits);
    ASSERT_TRUE(scope.editor.ResizeViewport(256, 192));
    const char* compute  = ZEN_ASSET_SHADER_PATH "spatial_alpha_hardware.comp.spv";
    const char* fragment = ZEN_ASSET_SHADER_PATH "spatial_alpha_hardware.frag.spv";
    rc::ShaderProgramManager::GetInstance().StoreProgram(
        ZEN_NEW() rc::ComputeFileSP(scope.device.Get(), "SpatialAlphaHardwareComputeSP", compute));
    rc::ShaderProgramManager::GetInstance().StoreProgram(ZEN_NEW() rc::HybridGraphicsSP(
        scope.device.Get(), "SpatialAlphaHardwareFragmentSP", ZEN_ASSET_SHADER_PATH "spatial_alpha.vert.spv", fragment));
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path()
        / ("zen_spatial_alpha_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code error;
    ASSERT_TRUE(std::filesystem::create_directory(directory, error)) << error.message();
    HeapVector<HeapVector<Vec4>> reference, packed;
    for (bool spatial : {false, true})
    {
        const std::string name = std::string("hardware-") + (execution == RHIExecutionMode::eInline ? "inline-" : "threaded-")
                               + (spatial ? "spatial" : "standalone");
        const std::filesystem::path  package = directory / (name + ".zenasset");
        zenasset::ProcessingSettings settings;
        settings.textures.spatialPages = spatial;
        asset::SceneLoadResult cooked;
        ASSERT_NO_FATAL_FAILURE(CookRenderingPackage(AssetFixture("spatial_alpha_scene.gltf"), package, settings, "desktop",
                                                     std::getenv("ZEN_EDITOR_SPATIAL_RAY_DIR"), cooked));
        ASSERT_EQ(cooked.asset->textures.size(), 4u);
        for (const asset::TextureAsset& texture : cooked.asset->textures)
        {
            EXPECT_EQ(texture.atlasRegion != asset::ImageRegion{}, spatial);
        }
        EXPECT_LT(cooked.asset->images.size(), spatial ? 4u : 5u);
        ASSERT_NO_FATAL_FAILURE(CaptureSpatialAlphaQueries(*scope.device, scope.editor, package, spatial ? packed : reference));
    }
    ASSERT_EQ(reference.size(), 4u);
    ASSERT_EQ(packed.size(), reference.size());
    uint32_t differences = 0;
    for (size_t capture = 0; capture < reference.size(); ++capture)
    {
        for (uint32_t ray = 0; ray < kSpatialRayCount; ++ray)
        {
            differences += reference[capture][ray] != packed[capture][ray] ? 1u : 0u;
        }
    }
    testing::Test::RecordProperty("paired_rays", kSpatialRayCount * 4);
    testing::Test::RecordProperty("ray_result_differences", differences);
    EXPECT_EQ(differences, 0u);
    std::filesystem::remove_all(directory, error);
    EXPECT_FALSE(error);
}

TEST_P(EditorRendering, CookedSpatialAlphaMatchesStandaloneHardwareRaysAndFragmentQueries)
{
    CompareCookedSpatialAlpha(GetParam());
}
