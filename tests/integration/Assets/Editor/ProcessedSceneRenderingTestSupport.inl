std::string AssetFixture(const char* name)
{
    return std::string(ZEN_ASSET_FIXTURES) + name;
}

// Shared by the package-based array and geometry rendering checks.

void CookRenderingPackage(const std::filesystem::path&        fixture,
                          const std::filesystem::path&        package,
                          const zenasset::ProcessingSettings& processing,
                          const char*                         profile,
                          const char*                         captureDirectory,
                          asset::SceneLoadResult&             cooked)
{
    const std::filesystem::path source = package.parent_path() / (package.stem().string() + fixture.extension().string());
    std::error_code             fileError;
    ASSERT_TRUE(std::filesystem::copy_file(fixture, source, fileError)) << fileError.message();
    asset::SceneImportRequest request{source.string()};
    request.cacheDirectory = (package.parent_path() / package.stem()).string();
    request.forceRecook    = true;
    request.profile        = profile;
    request.processing     = processing;
    cooked                 = asset::LoadSourceScene(request);
    ASSERT_TRUE(cooked.asset) << cooked.error;
    ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    std::filesystem::path published;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(request.cacheDirectory, fileError))
    {
        if (entry.path().extension() == ".zenasset")
        {
            ASSERT_TRUE(published.empty());
            published = entry.path();
        }
    }
    ASSERT_FALSE(fileError) << fileError.message();
    ASSERT_FALSE(published.empty());
    ASSERT_TRUE(std::filesystem::copy_file(published, package, fileError)) << fileError.message();
    const asset::SceneAssetFileResult metadata = asset::ReadSceneAssetMetadata(package.string());
    ASSERT_EQ(metadata.status, asset::SceneImportStatus::Success) << metadata.error;
    EXPECT_EQ(metadata.metadata.profile, request.profile);
    EXPECT_EQ(metadata.metadata.processing, request.processing);
    // Inputs are self-contained fixtures. Remove this copy before runtime load.
    ASSERT_TRUE(std::filesystem::remove(source, fileError)) << fileError.message();
    if (captureDirectory != nullptr)
    {
        ASSERT_TRUE(std::filesystem::copy_file(package, std::filesystem::path(captureDirectory) / package.filename(),
                                               std::filesystem::copy_options::overwrite_existing, fileError))
            << fileError.message();
    }
}

void ConfigureAssetRendering(EditorController& editor)
{
    rc::RenderingSettings settings = editor.GetRenderingState().GetDraft();
    settings.algorithm             = rc::RenderAlgorithm::ePBR;
    settings.debug.output          = rc::DebugOutput::eFinal;
    settings.lights.clear();
    settings.environment.intensity = 0.0f;
    settings.environment.skybox    = false;
    settings.lightMarkers          = false;
    settings.cameraLight.enabled   = false;
    rc::RenderingLight light;
    light.id                 = 1;
    light.light.type         = rc::SceneLightType::eDirectional;
    light.light.direction    = Vec3(0, 0, -1);
    light.light.intensity    = 3.0f;
    light.light.castsShadows = false;
    settings.lights.push_back(light);
    ASSERT_TRUE(editor.StageRenderingSettings(settings));
    ASSERT_TRUE(editor.UpdateRenderingSettings());
}
