// Package rendering after an actual vertex-storage reorder, including poses.
void CaptureCookedGeometry(rc::RenderDevice&                device,
                           EditorController&                editor,
                           const std::filesystem::path&     package,
                           HeapVector<HeapVector<uint8_t>>& captures)
{
    const char* captureDirectory = std::getenv("ZEN_EDITOR_GEOMETRY_CAPTURE_DIR");
    for (uint32_t clip = 0; clip < 3; ++clip)
    {
        ASSERT_TRUE(editor.Load(package.string())) << editor.GetError();
        EXPECT_FALSE(editor.CanRecook());
        ASSERT_NO_FATAL_FAILURE(ConfigureAssetRendering(editor));
        sg::AABB bounds;
        ASSERT_TRUE(editor.GetScene().GetSceneBounds(bounds));
        editor.GetCamera().Frame(bounds);
        editor.GetCamera().LookAlong(Vec3(0, 0, -1));
        // The inspector exposes const access to an owned mutable scene. Exercise
        // the renderer's normal animation API between frames, without a UI API.
        rc::RenderScene* scene = const_cast<rc::RenderScene*>(editor.GetViewport().GetRenderScene());
        ASSERT_NE(scene, nullptr);
        for (uint32_t pose = 0; pose < 5; ++pose)
        {
            SCOPED_TRACE(package.stem().string() + " clip " + std::to_string(clip) + " pose " + std::to_string(pose));
            ASSERT_TRUE(scene->SetAnimation(clip, false));
            ASSERT_TRUE(scene->AdvanceAnimation(float(pose) * 0.5f));
            ASSERT_TRUE(Frame(device, editor));
            EXPECT_TRUE(device.GetCurrentFrameRDG()->GetWarnings().empty());
            captures.push_back(ReadScenePixels(device, editor.GetViewport().GetRenderView().color));
            const HeapVector<uint8_t>& pixels = captures.back();
            ASSERT_EQ(pixels.size(), 256u * 192u * 4u);
            uint32_t colored = 0;
            for (size_t offset = 0; offset < pixels.size(); offset += 4)
            {
                colored += std::max({pixels[offset], pixels[offset + 1], pixels[offset + 2]})
                                     - std::min({pixels[offset], pixels[offset + 1], pixels[offset + 2]})
                                 > 8
                             ? 1u
                             : 0u;
            }
            EXPECT_GT(colored, 256u) << "Blank or neutral frames cannot establish the geometry comparison";
            if (captureDirectory != nullptr)
            {
                const std::filesystem::path path =
                    std::filesystem::path(captureDirectory)
                    / (package.stem().string() + "-c" + std::to_string(clip) + "-p" + std::to_string(pose) + ".ppm");
                std::ofstream output(path, std::ios::binary);
                ASSERT_TRUE(output.is_open());
                output << "P6\n256 192\n255\n";
                for (size_t offset = 0; offset < pixels.size(); offset += 4)
                {
                    output.write(reinterpret_cast<const char*>(pixels.data() + offset), 3);
                }
                ASSERT_TRUE(output.good());
            }
        }
        // Each clip must visibly animate; identical static frames are insufficient.
        EXPECT_FALSE(
            std::equal(captures[captures.size() - 5].begin(), captures[captures.size() - 5].end(), captures.back().begin()));
    }
}

TEST_P(EditorRendering, CookedVertexFetchPreservesSkinMorphAndTransparencyAtKeysAndMidpoints)
{
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path()
        / ("zen_geometry_rendering_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code fileError;
    ASSERT_TRUE(std::filesystem::create_directory(directory, fileError)) << fileError.message();
    const std::string            mode              = GetParam() == RHIExecutionMode::eInline ? "inline" : "threaded";
    const std::filesystem::path  referencePath     = directory / ("geometry-" + mode + "-reference.zenasset");
    const std::filesystem::path  optimizedPath     = directory / ("geometry-" + mode + "-optimized.zenasset");
    const std::filesystem::path  reversedPath      = directory / ("geometry-" + mode + "-reversed.zenasset");
    const char*                  capturesDirectory = std::getenv("ZEN_EDITOR_GEOMETRY_CAPTURE_DIR");
    asset::SceneLoadResult       reference, optimized, reversed;
    zenasset::ProcessingSettings processing;
    ASSERT_NO_FATAL_FAILURE(CookRenderingPackage(AssetFixture("geometry_scene.gltf"), referencePath, processing, "reference",
                                                 capturesDirectory, reference));
    ASSERT_NO_FATAL_FAILURE(CookRenderingPackage(AssetFixture("geometry_scene_reversed.gltf"), reversedPath, processing,
                                                 "reference", capturesDirectory, reversed));
    processing.geometry.optimizeVertexFetch = true;
    ASSERT_NO_FATAL_FAILURE(CookRenderingPackage(AssetFixture("geometry_scene.gltf"), optimizedPath, processing, "desktop",
                                                 capturesDirectory, optimized));
    ASSERT_EQ(reference.asset->vertices.size(), 6u);
    ASSERT_EQ(reference.asset->indices.size(), 6u);
    ASSERT_EQ(optimized.asset->vertices.size(), reference.asset->vertices.size());
    ASSERT_EQ(optimized.asset->indices.size(), reference.asset->indices.size());
    ASSERT_FALSE(std::equal(reference.asset->indices.begin(), reference.asset->indices.end(), optimized.asset->indices.begin()))
        << "The fixture must exercise an actual vertex-storage remap";
    ASSERT_EQ(optimized.asset->skins.size(), 1u);
    ASSERT_EQ(optimized.asset->animations.size(), 3u);
    ASSERT_EQ(optimized.asset->morphPrimitives.size(), 1u);
    ASSERT_FALSE(optimized.asset->extraVertexAttributes.empty());
    for (size_t corner = 0; corner < reference.asset->indices.size(); ++corner)
    {
        const asset::Vertex& before = reference.asset->vertices[reference.asset->indices[corner]];
        const asset::Vertex& after  = optimized.asset->vertices[optimized.asset->indices[corner]];
        EXPECT_EQ(before.pos, after.pos);
        EXPECT_EQ(before.normal, after.normal);
        EXPECT_EQ(before.uv0, after.uv0);
        EXPECT_EQ(before.uv1, after.uv1);
        EXPECT_EQ(before.color, after.color);
        EXPECT_EQ(before.joint0, after.joint0);
        EXPECT_EQ(before.weight0, after.weight0);
    }
    EditorRenderingScope scope(GetParam());
    ASSERT_TRUE(scope.editor.Init());
    ASSERT_TRUE(scope.editor.ResizeViewport(256, 192));
    HeapVector<HeapVector<uint8_t>> baseline, processed, reverseOrder;
    ASSERT_NO_FATAL_FAILURE(CaptureCookedGeometry(*scope.device, scope.editor, referencePath, baseline));
    ASSERT_NO_FATAL_FAILURE(CaptureCookedGeometry(*scope.device, scope.editor, optimizedPath, processed));
    ASSERT_NO_FATAL_FAILURE(CaptureCookedGeometry(*scope.device, scope.editor, reversedPath, reverseOrder));
    ASSERT_EQ(baseline.size(), 15u);
    ASSERT_EQ(processed.size(), baseline.size());
    ASSERT_EQ(reverseOrder.size(), baseline.size());
    for (size_t capture = 0; capture < baseline.size(); ++capture)
    {
        SCOPED_TRACE(mode + " frame " + std::to_string(capture));
        EXPECT_TRUE(std::equal(baseline[capture].begin(), baseline[capture].end(), processed[capture].begin()));
        EXPECT_FALSE(std::equal(baseline[capture].begin(), baseline[capture].end(), reverseOrder[capture].begin()))
            << "The transparency control must reveal a triangle-order change";
    }
    std::filesystem::remove_all(directory, fileError);
    EXPECT_FALSE(fileError) << fileError.message();
}
