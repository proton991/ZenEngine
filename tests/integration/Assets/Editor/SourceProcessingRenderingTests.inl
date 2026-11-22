TEST_P(EditorRendering, FbxScenesRenderThroughPbrAndVoxelGi)
{
    UniquePtr<rc::RenderDevice> device = CreateDevice(GetParam());
    EditorController            editor(*device);
    ASSERT_TRUE(editor.Init());
    ASSERT_TRUE(editor.ResizeViewport(96, 96));
    const char* fixtures[] = {"FBX/maya_cube_7500_binary.fbx", "FBX/blender_293_embedded_textures_7400_binary.fbx",
                              "FBX/max_instanced_material_7700_ascii.fbx", "FBX/blender_331_static_blend_shape_7400_binary.fbx",
                              "FBX/combined_morph_skin_ascii.fbx"};
    for (const char* fixture : fixtures)
    {
        SCOPED_TRACE(fixture);
        ASSERT_TRUE(editor.Load(std::string(ZEN_EDITOR_SHARED_FIXTURES) + fixture)) << editor.GetError();
        editor.GetCamera().GetCamera().SetPose(Vec3(0, 0, 4), Vec3(0));
        for (rc::RenderAlgorithm algorithm : {rc::RenderAlgorithm::ePBR, rc::RenderAlgorithm::eVoxelGI})
        {
            rc::RenderingSettings settings     = editor.GetRenderingState().GetDraft();
            settings.algorithm                 = algorithm;
            settings.debug.output              = rc::DebugOutput::eFinal;
            settings.gi.resolution             = 64;
            settings.gi.shadowMapResolution    = 256;
            settings.gi.cone.indirectIntensity = 0;
            settings.environment.intensity     = 0;
            settings.environment.skybox        = false;
            settings.lightMarkers              = false;
            settings.cameraLight.enabled       = true;
            settings.cameraLight.range         = 10;
            settings.cameraLight.intensity     = 2;
            ASSERT_TRUE(editor.StageRenderingSettings(settings, true));
            ASSERT_TRUE(editor.UpdateRenderingSettings());
            const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            do
            {
                ASSERT_TRUE(Frame(*device, editor));
                device->WaitForIdle();
            } while (editor.GetViewport().GetSnapshot().status.effective != algorithm
                     && std::chrono::steady_clock::now() < deadline);
            ASSERT_EQ(editor.GetViewport().GetSnapshot().status.effective, algorithm);
            EXPECT_TRUE(device->GetCurrentFrameRDG()->GetWarnings().empty());
            const HeapVector<uint8_t> pixels = ReadScenePixels(*device, editor.GetViewport().GetRenderView().color);
            uint64_t                  energy = 0;
            for (size_t index = 0; index < pixels.size(); ++index)
            {
                if (index % 4 != 3)
                {
                    energy += pixels[index];
                }
            }
            EXPECT_GT(energy, 1000u);
            EXPECT_GT(editor.GetViewport().GetRenderScene()->GetVoxelTriangleCount(), 0u);
        }
    }
    DestroyDevice(*device, editor);
}

TEST_P(EditorRendering, SourceProfilesRecookSnapshotsAndRollbackPreserveThePublishedScene)
{
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path()
        / ("zen_editor_recook_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code fileError;
    std::filesystem::create_directory(directory, fileError);
    ASSERT_FALSE(fileError);
    UniquePtr<rc::RenderDevice> device = CreateDevice(GetParam());
    EditorController            editor(*device);
    ASSERT_TRUE(editor.Init());
    ASSERT_TRUE(editor.ResizeViewport(64, 64));
    EXPECT_FALSE(editor.CanRecook());
    EXPECT_FALSE(editor.GetSourceProcessing().draft.desktop);
    asset::SceneImportRequest initial{std::string(ZEN_EDITOR_SHARED_FIXTURES) + "texture_roles.gltf"};
    initial.cacheDirectory = (directory / "cache").string();
    ASSERT_TRUE(editor.Load(initial)) << editor.GetError();
    ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    ASSERT_TRUE(editor.CanRecook());
    EXPECT_FALSE(editor.GetSourceProcessing().applied.desktop);
    SceneProcessingSettings desktop;
    desktop.desktop                                    = true;
    desktop.settings.textures.compressBC7              = true;
    desktop.settings.textures.generateMipmaps          = true;
    SceneProcessingSettings invalid                    = desktop;
    invalid.settings.geometry.positionQuantizationBits = 25;
    EXPECT_FALSE(editor.StageSourceProcessing(invalid));
    EXPECT_FALSE(editor.GetSourceProcessing().draft.desktop);
    EXPECT_FALSE(editor.GetSourceProcessing().error.empty());
    ASSERT_TRUE(editor.StageSourceProcessing(desktop));
    ASSERT_TRUE(editor.RequestRecook());
    EXPECT_TRUE(editor.GetLoadState().recook);
    EXPECT_FALSE(editor.GetActions().IsEnabled(actions::Recook));
    // Editing the next request cannot change the queued snapshot.
    ASSERT_TRUE(editor.StageSourceProcessing({}));
    ASSERT_TRUE(editor.UpdateSceneLoad());
    WaitForImport(editor);
    ASSERT_EQ(editor.GetLoadState().stage, SceneLoadStage::Preparing) << editor.GetError();
    ASSERT_TRUE(editor.UpdateSceneLoad());
    ASSERT_TRUE(Frame(*device, editor));
    ASSERT_TRUE(editor.UpdateSceneLoad());
    ASSERT_TRUE(editor.GetSourceProcessing().applied.desktop);
    EXPECT_EQ(editor.GetSourceProcessing().applied.settings, desktop.settings);
    EXPECT_FALSE(editor.GetSourceProcessing().cacheHit);
    uint32_t compressed = 0;
    for (const SceneAssetItem& item : editor.GetScene().GetAssets().Query(""))
    {
        const sg::Texture* texture = editor.GetScene().GetAssets().ResolveTexture(item.id);
        if (texture != nullptr
            && (texture->format == asset::Format::BC7_UNORM_BLOCK || texture->format == asset::Format::BC7_SRGB_BLOCK))
        {
            ++compressed;
        }
    }
    EXPECT_GT(compressed, 0u);
    editor.UpdateSceneLoad();
    for (SceneLoadStage stage :
         {SceneLoadStage::Queued, SceneLoadStage::Reading, SceneLoadStage::Preparing, SceneLoadStage::Publishing})
    {
        const uint64_t         generation = editor.GetScene().GetGeneration();
        const rc::RenderScene* retained   = editor.GetViewport().GetRenderScene();
        ASSERT_TRUE(editor.RequestRecook());
        if (stage != SceneLoadStage::Queued)
        {
            editor.UpdateSceneLoad();
        }
        if (stage == SceneLoadStage::Preparing || stage == SceneLoadStage::Publishing)
        {
            WaitForImport(editor);
            ASSERT_EQ(editor.GetLoadState().stage, SceneLoadStage::Preparing) << editor.GetError();
        }
        if (stage == SceneLoadStage::Publishing)
        {
            editor.UpdateSceneLoad();
        }
        ASSERT_TRUE(editor.CancelSceneLoad());
        if (stage == SceneLoadStage::Reading)
        {
            WaitForImport(editor);
        }
        EXPECT_EQ(editor.GetLoadState().stage, SceneLoadStage::Cancelled);
        EXPECT_EQ(editor.GetScene().GetGeneration(), generation);
        EXPECT_EQ(editor.GetViewport().GetRenderScene(), retained);
        EXPECT_TRUE(editor.GetSourceProcessing().applied.desktop);
        EXPECT_TRUE(Frame(*device, editor));
        editor.UpdateSceneLoad();
    }
    const uint64_t            generation = editor.GetScene().GetGeneration();
    const rc::RenderScene*    retained   = editor.GetViewport().GetRenderScene();
    asset::SceneImportRequest failing    = initial;
    failing.forceRecook                  = true;
    failing.cacheDirectory               = (directory / "blocked").string();
    {
        std::ofstream blocked(failing.cacheDirectory);
        blocked << "not a directory";
    }
    ASSERT_TRUE(editor.RequestLoad(failing));
    editor.UpdateSceneLoad();
    WaitForImport(editor);
    EXPECT_EQ(editor.GetLoadState().stage, SceneLoadStage::Failed);
    EXPECT_FALSE(editor.GetLastLoadDiagnostics().empty());
    EXPECT_EQ(editor.GetScene().GetGeneration(), generation);
    EXPECT_EQ(editor.GetViewport().GetRenderScene(), retained);
    EXPECT_TRUE(editor.GetSourceProcessing().applied.desktop);
    EXPECT_TRUE(Frame(*device, editor));
    editor.DismissLoadError();
    std::string referencePackage;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory / "cache"))
    {
        const asset::SceneAssetFileResult metadata = asset::ReadSceneAssetMetadata(entry.path().string());
        if (metadata.metadata.profile == "reference")
        {
            referencePackage = entry.path().string();
        }
    }
    ASSERT_FALSE(referencePackage.empty());
    ASSERT_TRUE(editor.StageSourceProcessing(desktop));
    ASSERT_TRUE(editor.Load(referencePackage)) << editor.GetError();
    EXPECT_FALSE(editor.CanRecook());
    EXPECT_TRUE(editor.GetSourceProcessing().sourcePath.empty());
    for (const SceneAssetItem& item : editor.GetScene().GetAssets().Query(""))
    {
        const sg::Texture* texture = editor.GetScene().GetAssets().ResolveTexture(item.id);
        if (texture != nullptr)
        {
            EXPECT_NE(texture->format, asset::Format::BC7_UNORM_BLOCK);
            EXPECT_NE(texture->format, asset::Format::BC7_SRGB_BLOCK);
        }
    }
    DestroyDevice(*device, editor);
    EXPECT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    std::filesystem::remove_all(directory, fileError);
    EXPECT_FALSE(fileError);
}

// Run against an installed CLI batch output authored from recook_textured.gltf.
// The external command is intentionally owned by the verification harness.
TEST_P(EditorRendering, BatchPackageMatchesEditorSelectedSourceProfile)
{
    const char* batchPath = std::getenv("ZEN_EDITOR_BATCH_PACKAGE");
    if (batchPath == nullptr)
    {
        GTEST_SKIP() << "Set ZEN_EDITOR_BATCH_PACKAGE to the desktop batch package for recook_textured.gltf";
    }
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path()
        / ("zen_editor_batch_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code fileError;
    std::filesystem::create_directory(directory, fileError);
    ASSERT_FALSE(fileError);
    UniquePtr<rc::RenderDevice> device = CreateDevice(GetParam());
    EditorController            editor(*device);
    ASSERT_TRUE(editor.Init());
    ASSERT_TRUE(editor.ResizeViewport(64, 64));
    asset::SceneImportRequest initial{AssetFixture("recook_textured.gltf")};
    initial.cacheDirectory = (directory / "cache").string();
    ASSERT_TRUE(editor.Load(initial)) << editor.GetError();
    ASSERT_TRUE(asset::WaitForSceneAssetCacheWrites().empty());
    SceneProcessingSettings desktop;
    desktop.desktop                               = true;
    desktop.settings.geometry.optimizeVertexFetch = true;
    desktop.settings.textures.compressBC7         = true;
    desktop.settings.textures.generateMipmaps     = true;
    ASSERT_TRUE(editor.StageSourceProcessing(desktop));
    ASSERT_TRUE(editor.RequestRecook());
    editor.UpdateSceneLoad();
    WaitForImport(editor);
    ASSERT_EQ(editor.GetLoadState().stage, SceneLoadStage::Preparing) << editor.GetError();
    editor.UpdateSceneLoad();
    ASSERT_TRUE(Frame(*device, editor));
    editor.UpdateSceneLoad();
    ASSERT_EQ(editor.GetLoadState().stage, SceneLoadStage::Complete) << editor.GetError();
    EXPECT_EQ(editor.GetSourceProcessing().applied.settings, desktop.settings);
    asset::SceneAssetFileResult cached;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory / "cache"))
    {
        const asset::SceneAssetFileResult metadata = asset::ReadSceneAssetMetadata(entry.path().string());
        if (metadata.metadata.profile == "desktop")
        {
            cached = asset::ReadSceneAssetFile(entry.path().string());
        }
    }
    ASSERT_TRUE(cached.asset) << cached.error;
    const asset::SceneAssetFileResult batch = asset::ReadSceneAssetFile(batchPath);
    ASSERT_TRUE(batch.asset) << batch.error;
    EXPECT_EQ(batch.metadata.profile, "desktop");
    EXPECT_EQ(batch.metadata.processing, desktop.settings);
    // Canonical serialization compares every engine scene record, including
    // geometry, material references, compressed blocks and retained RGBA owners.
    const std::string actual   = (directory / "editor-canonical.zenasset").string();
    const std::string expected = (directory / "batch-canonical.zenasset").string();
    ASSERT_EQ(asset::WriteSceneAssetFile(actual, *cached.asset).status, asset::SceneImportStatus::Success);
    ASSERT_EQ(asset::WriteSceneAssetFile(expected, *batch.asset).status, asset::SceneImportStatus::Success);
    std::ifstream actualFile(actual, std::ios::binary);
    std::ifstream expectedFile(expected, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(actualFile), std::istreambuf_iterator<char>()),
              std::string(std::istreambuf_iterator<char>(expectedFile), std::istreambuf_iterator<char>()));
    actualFile.close();
    expectedFile.close();
    EXPECT_EQ(cached.asset->indices.front(), 0u);
    EXPECT_EQ(cached.asset->vertices.front().pos, Vec4(0, 1, 0, 1));
    bool compressed    = false;
    bool retained      = false;
    bool refinedNormal = false;
    for (const asset::ImageAsset& image : cached.asset->images)
    {
        compressed |= image.pixels
                   && (image.pixels->format == asset::Format::BC7_UNORM_BLOCK
                       || image.pixels->format == asset::Format::BC7_SRGB_BLOCK);
        retained   |= image.pixels && image.pixels->format == asset::Format::R8G8B8A8_UNORM;
        if (image.pixels && image.pixels->width == 8 && image.pixels->height == 8)
        {
            EXPECT_EQ(image.pixels->format, asset::Format::BC7_UNORM_BLOCK);
            EXPECT_EQ(image.pixels->mips.size(), 4u);
            refinedNormal = true;
        }
    }
    EXPECT_TRUE(compressed);
    EXPECT_TRUE(retained);
    EXPECT_TRUE(refinedNormal);
    const HeapVector<asset::SceneImportDiagnostic>& diagnostics = editor.GetImportDiagnostics();
    ASSERT_EQ(diagnostics.size(), batch.diagnostics.size());
    bool fallback = false;
    for (size_t index = 0; index < diagnostics.size(); ++index)
    {
        EXPECT_EQ(diagnostics[index].severity, batch.diagnostics[index].severity);
        EXPECT_EQ(diagnostics[index].object, batch.diagnostics[index].object);
        EXPECT_EQ(diagnostics[index].message, batch.diagnostics[index].message);
        fallback |= diagnostics[index].message.find("normal scale retains") != std::string::npos;
    }
    EXPECT_TRUE(fallback);
    editor.UpdateSceneLoad();
    ASSERT_TRUE(editor.Load(batchPath)) << editor.GetError();
    EXPECT_FALSE(editor.CanRecook());
    EXPECT_TRUE(Frame(*device, editor));
    DestroyDevice(*device, editor);
    std::filesystem::remove_all(directory, fileError);
    EXPECT_FALSE(fileError);
}
