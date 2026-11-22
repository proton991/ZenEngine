// Cooked spatial/reference GI checks using deterministic learned-guide capture.
void CaptureCookedSpatialGI(const std::filesystem::path& package,
                            RHIExecutionMode             mode,
                            bool                         hardware,
                            bool                         alpha,
                            const std::string&           tag,
                            HeapVector<uint8_t>&         pixels)
{
    EditorRenderingScope      scope(mode, hardware);
    EditorRenderingValidation validation;
    ASSERT_TRUE(scope.editor.Init());
    ASSERT_TRUE(scope.editor.Load(package.string())) << scope.editor.GetError();
    EXPECT_FALSE(scope.editor.CanRecook());
    ASSERT_TRUE(scope.editor.ResizeViewport(256, 192));
    ASSERT_EQ(scope.device->GetGPUInfo().rayQuery.IsUsable(), hardware);
    ASSERT_NO_FATAL_FAILURE(ConfigureAssetRendering(scope.editor));
    rc::RenderingSettings settings  = scope.editor.GetRenderingState().GetDraft();
    settings.algorithm              = rc::RenderAlgorithm::eVoxelGI;
    settings.gi.resolution          = 64;
    settings.gi.shadowMapResolution = 128;
    settings.gi.cone                = rc::VoxelGISettings{};
    settings.gi.cone.rayProvider =
        hardware ? rc::VoxelGISettings::RayProvider::Hardware : rc::VoxelGISettings::RayProvider::Voxel;
    settings.environment.intensity        = 1.0f;
    settings.lights[0].light.castsShadows = true;
    if (alpha)
    {
        settings.lights[0].light.direction = Vec3(0, 0, 1);
    }
    ASSERT_TRUE(scope.editor.StageRenderingSettings(settings, true));
    ASSERT_TRUE(scope.editor.UpdateRenderingSettings()) << scope.editor.GetRenderingState().GetError();
    sg::AABB bounds;
    ASSERT_TRUE(scope.editor.GetScene().GetSceneBounds(bounds));
    scope.editor.GetCamera().Frame(bounds);
    scope.editor.GetCamera().LookAlong(Vec3(0, 0, alpha ? 1 : -1));
    scope.editor.GetCamera().OrbitBy(Vec2(0.2f, 0.1f));
    scope.device->GetRendererServer()->RequestDeferredLightingRenderer()->SetDeterministicGuideCapture(true);
    const rc::VoxelGIRenderer&                  gi       = *scope.device->GetRendererServer()->RequestVoxelGI();
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    do
    {
        ASSERT_TRUE(Frame(*scope.device, scope.editor));
        scope.device->WaitForIdle();
    } while (!(hardware ? gi.UsesHardwareQueries() : (gi.UsesComputeQueries() && !gi.IsRefiningField()))
             && std::chrono::steady_clock::now() < deadline);
    ASSERT_TRUE(hardware ? gi.UsesHardwareQueries() : (gi.UsesComputeQueries() && !gi.IsRefiningField()));
    for (uint32_t frame = 0; frame < 16; ++frame)
    {
        ASSERT_TRUE(Frame(*scope.device, scope.editor));
    }
    pixels = ReadScenePixels(*scope.device, scope.editor.GetViewport().GetRenderView().color);
    ASSERT_EQ(pixels.size(), 256u * 192u * 4u);
    uint32_t colored = 0;
    for (size_t offset = 0; offset < pixels.size(); offset += 4)
    {
        const uint8_t low   = std::min(pixels[offset], std::min(pixels[offset + 1], pixels[offset + 2]));
        const uint8_t high  = std::max(pixels[offset], std::max(pixels[offset + 1], pixels[offset + 2]));
        colored            += high - low > 30 ? 1u : 0u;
    }
    EXPECT_GT(colored, 512u);
    const char* captureDirectory = std::getenv("ZEN_EDITOR_SPATIAL_GI_DIR");
    if (captureDirectory != nullptr)
    {
        std::ofstream output(std::filesystem::path(captureDirectory) / (tag + ".ppm"), std::ios::binary);
        ASSERT_TRUE(output.is_open());
        output << "P6\n256 192\n255\n";
        for (size_t offset = 0; offset < pixels.size(); offset += 4)
        {
            output.write(reinterpret_cast<const char*>(pixels.data() + offset), 3);
        }
        ASSERT_TRUE(output.good());
    }
    // Disabling GI must change the actual frame; active provider flags alone
    // cannot establish that these captures exercise GI material output.
    settings.algorithm = rc::RenderAlgorithm::ePBR;
    ASSERT_TRUE(scope.editor.StageRenderingSettings(settings, true));
    ASSERT_TRUE(scope.editor.UpdateRenderingSettings());
    ASSERT_TRUE(Frame(*scope.device, scope.editor));
    const HeapVector<uint8_t> control = ReadScenePixels(*scope.device, scope.editor.GetViewport().GetRenderView().color);
    ASSERT_EQ(control.size(), pixels.size());
    size_t changes = 0;
    for (size_t channel = 0; channel < pixels.size(); ++channel)
    {
        changes += control[channel] != pixels[channel] ? 1u : 0u;
    }
    EXPECT_GT(changes, 100u);
}

void CompareCookedSpatialGI(RHIExecutionMode mode, bool hardware)
{
    if (hardware)
    {
        EditorRenderingScope capability(mode, true);
        if (!capability.device->GetGPUInfo().rayQuery.IsUsable())
        {
            GTEST_SKIP() << "Hardware ray queries are unavailable";
        }
    }
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path()
        / ("zen_spatial_gi_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code error;
    ASSERT_TRUE(std::filesystem::create_directory(directory, error)) << error.message();
    for (bool alpha : {false, true})
    {
        const std::string name = std::string(hardware ? "hardware-" : "compute-")
                               + (mode == RHIExecutionMode::eInline ? "inline-" : "threaded-") + (alpha ? "alpha" : "material");
        HeapVector<HeapVector<uint8_t>> captures;
        std::filesystem::path           reference;
        for (uint32_t run = 0; run < 3; ++run)
        {
            const std::string     suffix  = run == 0 ? "-standalone" : run == 1 ? "-spatial" : "-repeat";
            std::filesystem::path package = directory / (name + suffix + ".zenasset");
            if (run < 2)
            {
                zenasset::ProcessingSettings settings;
                settings.textures.spatialPages = run == 1;
                asset::SceneLoadResult cooked;
                ASSERT_NO_FATAL_FAILURE(
                    CookRenderingPackage(AssetFixture(alpha ? "spatial_alpha_scene.gltf" : "spatial_scene.gltf"), package,
                                         settings, "desktop", std::getenv("ZEN_EDITOR_SPATIAL_GI_DIR"), cooked));
                uint32_t regions = 0;
                for (const asset::TextureAsset& texture : cooked.asset->textures)
                {
                    regions += texture.atlasRegion != asset::ImageRegion{} ? 1u : 0u;
                }
                ASSERT_EQ(regions, run == 1 ? (alpha ? 4u : 6u) : 0u);
                if (run == 0)
                {
                    reference = package;
                }
            }
            else
            {
                package = reference;
            }
            captures.emplace_back();
            ASSERT_NO_FATAL_FAILURE(CaptureCookedSpatialGI(package, mode, hardware, alpha, name + suffix, captures.back()));
        }
        for (uint32_t capture = 1; capture < 3; ++capture)
        {
            ASSERT_EQ(captures[0].size(), captures[capture].size());
            size_t   changes = 0;
            uint32_t maximum = 0;
            for (size_t channel = 0; channel < captures[0].size(); ++channel)
            {
                const uint32_t difference =
                    static_cast<uint32_t>(std::abs(int(captures[0][channel]) - int(captures[capture][channel])));
                changes += difference > 0 ? 1u : 0u;
                maximum  = std::max(maximum, difference);
            }
            std::printf("GI parity %s comparison %u: changed %zu maximum %u\n", name.c_str(), capture, changes, maximum);
            if (!hardware && alpha && capture == 1)
            {
                // The compute field uses conservative alpha bounds in physical atlas texels.
                // Packing can move an edge by sub-texel precision. Bound both severity and extent.
                EXPECT_LE(maximum, 2u) << name;
                EXPECT_LE(changes, captures[0].size() / 100u) << name;
            }
            else
            {
                EXPECT_EQ(maximum, 0u) << name << " comparison " << capture << " changed channels " << changes;
            }
        }
    }
    std::filesystem::remove_all(directory, error);
    EXPECT_FALSE(error);
}

TEST_P(EditorRendering, CookedSpatialGIWithComputeQueriesMatchesStandalone)
{
    CompareCookedSpatialGI(GetParam(), false);
}
TEST_P(EditorRendering, CookedSpatialGIWithHardwareQueriesMatchesStandalone)
{
    CompareCookedSpatialGI(GetParam(), true);
}
