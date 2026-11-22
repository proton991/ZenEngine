// Included inside the editor rendering fixture's anonymous namespace.
void CookPageRenderingPackage(const std::filesystem::path& package,
                              bool                         compressed,
                              bool                         arrays,
                              bool                         minification,
                              bool                         spatial)
{
    zenasset::ProcessingSettings processing;
    processing.textures.generateMipmaps      = !spatial;
    processing.textures.compressBC7          = compressed;
    processing.textures.compressBC5Normals   = compressed;
    processing.textures.compressBC4Occlusion = compressed;
    processing.textures.arrayPages           = arrays && !spatial;
    processing.textures.spatialPages         = arrays && spatial;
    asset::SceneLoadResult cooked;
    ASSERT_NO_FATAL_FAILURE(CookRenderingPackage(
        AssetFixture(spatial ? "spatial_scene.gltf" : (minification ? "array_scene_minification.gltf" : "array_scene.gltf")),
        package, processing, "desktop", std::getenv("ZEN_EDITOR_ARRAY_CAPTURE_DIR"), cooked));
    EXPECT_EQ(cooked.asset->nodes.size(), 5u);
    EXPECT_EQ(cooked.asset->meshes.size(), 3u);
    EXPECT_EQ(cooked.asset->textures.size(), 7u);
    EXPECT_EQ(cooked.asset->images.size(), arrays ? 4u : 7u);
    uint32_t pages = 0;
    bool     bc7 = false, bc5 = false, bc4 = false;
    for (const asset::ImageAsset& image : cooked.asset->images)
    {
        ASSERT_TRUE(image.pixels);
        pages += image.pixels->arrayLayers > 1 ? 1u : 0u;
        bc7 |= image.pixels->format == asset::Format::BC7_UNORM_BLOCK || image.pixels->format == asset::Format::BC7_SRGB_BLOCK;
        bc5 |= image.pixels->format == asset::Format::BC5_UNORM_BLOCK;
        bc4 |= image.pixels->format == asset::Format::BC4_UNORM_BLOCK;
    }
    EXPECT_EQ(pages, arrays && !spatial ? 3u : 0u);
    uint32_t regions = 0;
    for (const asset::TextureAsset& texture : cooked.asset->textures)
    {
        regions += texture.atlasRegion != asset::ImageRegion{} ? 1u : 0u;
    }
    EXPECT_EQ(regions, arrays && spatial ? 6u : 0u);
    EXPECT_EQ(bc7, compressed);
    EXPECT_EQ(bc5, compressed);
    EXPECT_EQ(bc4, compressed);
}

void CapturePageRenderingPackage(rc::RenderDevice&                device,
                                 EditorController&                editor,
                                 const std::filesystem::path&     package,
                                 bool                             arrays,
                                 bool                             minification,
                                 HeapVector<HeapVector<uint8_t>>& captures,
                                 bool                             spatial)
{
    // Package input selects the source-free reader path after the cook has ended.
    ASSERT_TRUE(editor.Load(package.string())) << editor.GetError();
    EXPECT_FALSE(editor.CanRecook());
    const rc::RenderScene* scene = editor.GetViewport().GetRenderScene();
    ASSERT_NE(scene, nullptr);
    const HeapVector<RHITextureView*>& views = scene->GetSceneTextureViews();
    // The runtime appends five fallback textures after the seven authored views.
    ASSERT_EQ(views.size(), 12u);
    HeapVector<RHITexture*> images;
    for (size_t index = 0; index < 7; ++index)
    {
        RHITextureView* view = views[index];
        ASSERT_NE(view, nullptr);
        EXPECT_EQ(view->GetSubResourceRange().layerCount, 1u);
        if (std::find(images.begin(), images.end(), view->GetTexture()) == images.end())
        {
            images.push_back(view->GetTexture());
        }
    }
    EXPECT_EQ(images.size(), arrays ? 4u : 7u);
    for (uint32_t texture = 0; texture < 6; texture += 2)
    {
        EXPECT_EQ(views[texture]->GetTexture() == views[texture + 1]->GetTexture(), arrays);
        EXPECT_EQ(views[texture]->GetTexture()->GetArrayLayers(), arrays && !spatial ? 2u : 1u);
        EXPECT_EQ(views[texture + 1]->GetSubResourceRange().baseArrayLayer, arrays && !spatial ? 1u : 0u);
    }
    EXPECT_EQ(views[6]->GetTexture()->GetArrayLayers(), 1u);
    EXPECT_EQ(scene->GetSceneSamplers().size(), 2u);
    if (spatial && arrays)
    {
        rc::RenderScene& mutableScene = *const_cast<rc::RenderScene*>(scene);
        ASSERT_FALSE(scene->GetMaterialsData().empty());
        const sg::MaterialData original = scene->GetMaterialsData()[0];
        EXPECT_FLOAT_EQ(original.textureSampling.x, std::exp2(static_cast<float>(device.GetGPUInfo().subTexelPrecisionBits)));
        EXPECT_EQ(original.textureSampling, device.GetTextureSamplingParameters());
        EXPECT_NE(original.textureSampling.y, 0);
        ASSERT_NE(original.textureTransforms[0].atlasRegion, glm::uvec4(0));
        ASSERT_NE(original.featureTextures[6].transform.atlasRegion, glm::uvec4(0));
        sg::MaterialData invalid                    = original;
        invalid.textureTransforms[0].atlasRegion.x += 4;
        EXPECT_FALSE(mutableScene.UpdateMaterial(0, invalid));
        invalid                                          = original;
        invalid.featureTextures[6].transform.atlasRegion = glm::uvec4(0);
        EXPECT_FALSE(mutableScene.UpdateMaterial(0, invalid));
        invalid                             = original;
        invalid.textureTransforms[0].row0.w = 0;
        EXPECT_FALSE(mutableScene.UpdateMaterial(0, invalid));
        invalid                              = original;
        invalid.textureTransforms[0].row1.w += 1;
        EXPECT_FALSE(mutableScene.UpdateMaterial(0, invalid));
        invalid                                     = original;
        invalid.featureTextures[6].transform.row1.w = -1;
        EXPECT_FALSE(mutableScene.UpdateMaterial(0, invalid));
        EXPECT_EQ(std::memcmp(&scene->GetMaterialsData()[0], &original, sizeof(original)), 0);
        sg::MaterialData updated = original;
        updated.textureSampling  = Vec4(1, 0, 0, 0); // Callers cannot replace the device precision or rounding.
        EXPECT_TRUE(mutableScene.UpdateMaterial(0, updated));
        EXPECT_EQ(std::memcmp(&scene->GetMaterialsData()[0], &original, sizeof(original)), 0);
    }
    const char* captureDirectory = std::getenv("ZEN_EDITOR_ARRAY_CAPTURE_DIR");
    if (captureDirectory != nullptr)
    {
        std::ofstream metrics(std::filesystem::path(captureDirectory) / (package.stem().string() + "-resources.json"));
        ASSERT_TRUE(metrics.is_open());
        metrics << "{\"authored_images\":" << images.size() << ",\"texture_bindings\":" << views.size()
                << ",\"sampler_bindings\":" << scene->GetSceneSamplers().size() << ",\"resources\":[";
        uint64_t allocationBytes = 0;
        for (size_t index = 0; index < images.size(); ++index)
        {
            // This fixture creates a Vulkan device. Query its actual VMA resource
            // allocation, excluding unused pool blocks and descriptor/view memory.
            const RHITexture* texture = images[index];
            const uint64_t    bytes   = texture->GetAllocationSize();
            EXPECT_GT(bytes, 0u);
            allocationBytes += bytes;
            metrics << (index == 0 ? "" : ",") << "{\"format\":" << uint32_t(texture->GetFormat())
                    << ",\"width\":" << texture->GetWidth() << ",\"height\":" << texture->GetHeight()
                    << ",\"layers\":" << texture->GetArrayLayers() << ",\"mips\":" << texture->GetNumMipmaps()
                    << ",\"allocation_bytes\":" << bytes << "}";
        }
        metrics << "],\"texture_allocation_bytes\":" << allocationBytes << "}\n";
        ASSERT_TRUE(metrics.good());
    }

    ASSERT_NO_FATAL_FAILURE(ConfigureAssetRendering(editor));
    sg::AABB bounds;
    ASSERT_TRUE(editor.GetScene().GetSceneBounds(bounds));
    struct CameraCase
    {
        Vec2     orbit;
        float    dolly;
        uint32_t minimumColoredPixels;
    };
    const CameraCase cameras[]   = {{Vec2(0), 0, 512},  {Vec2(0.65f, 0.2f), 0, 512}, {Vec2(0), -6, 64},
                                    {Vec2(0), -12, 16}, {Vec2(1.35f, 0.2f), 0, 128}, {Vec2(0), -24, 0}};
    const uint32_t   cameraCount = minification ? 6u : 2u;
    for (int32_t variant : {-1, 0})
    {
        // The inspector exposes a const view; the owned scene is mutable. Exercise
        // its existing variant API between frames without adding a test-only UI API.
        ASSERT_TRUE(const_cast<rc::RenderScene*>(scene)->SetMaterialVariant(variant));
        for (uint32_t angle = 0; angle < cameraCount; ++angle)
        {
            editor.GetCamera().Frame(bounds);
            editor.GetCamera().LookAlong(Vec3(0, 0, -1));
            editor.GetCamera().OrbitBy(cameras[angle].orbit);
            CameraInput motion;
            // Keep the single-level clearcoat fixture visible in the farthest view.
            motion.dolly = spatial && angle == 5 ? -18.0f : cameras[angle].dolly;
            editor.GetCamera().Apply(motion);
            ASSERT_TRUE(Frame(device, editor));
            captures.push_back(ReadScenePixels(device, editor.GetViewport().GetRenderView().color));
            const HeapVector<uint8_t>& pixels = captures.back();
            ASSERT_EQ(pixels.size(), 256u * 192u * 4u);
            uint32_t coloredPixels = 0;
            for (size_t offset = 0; offset < pixels.size(); offset += 4)
            {
                const uint8_t low   = std::min(pixels[offset], std::min(pixels[offset + 1], pixels[offset + 2]));
                const uint8_t high  = std::max(pixels[offset], std::max(pixels[offset + 1], pixels[offset + 2]));
                coloredPixels      += high - low > 30 ? 1u : 0u;
            }
            EXPECT_GT(coloredPixels, cameras[angle].minimumColoredPixels)
                << "A blank image cannot establish parity; camera " << angle;
            if (captureDirectory != nullptr)
            {
                const std::filesystem::path path =
                    std::filesystem::path(captureDirectory)
                    / (package.stem().string() + "-v" + std::to_string(variant) + "-a" + std::to_string(angle) + ".ppm");
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
    }
    ASSERT_EQ(captures.size(), cameraCount * 2u);
    EXPECT_FALSE(std::equal(captures[0].begin(), captures[0].end(), captures[cameraCount].begin()))
        << "The swapped material variant must change the rendered scene";
}

void CompareCookedPageScenes(RHIExecutionMode executionMode, bool minification, bool spatial = false)
{
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path()
        / ("zen_array_rendering_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code fileError;
    ASSERT_TRUE(std::filesystem::create_directory(directory, fileError)) << fileError.message();
    EditorRenderingScope scope(executionMode);
    rc::RenderDevice*    device = scope.device.Get();
    EditorController&    editor = scope.editor;
    ASSERT_TRUE(editor.Init());
    ASSERT_TRUE(editor.ResizeViewport(256, 192));
    const std::string mode     = executionMode == RHIExecutionMode::eInline ? "inline" : "threaded";
    const uint32_t    profiles = spatial ? 1u : 2u;
    for (uint32_t profile = 0; profile < profiles; ++profile)
    {
        const bool        compressed = profile != 0;
        const std::string name =
            std::string(spatial ? "spatial-" : (minification ? "mips-" : "")) + (compressed ? "bc-" : "raw-") + mode;
        HeapVector<HeapVector<uint8_t>> reference;
        HeapVector<HeapVector<uint8_t>> packed;
        for (bool arrays : {false, true})
        {
            const std::filesystem::path package = directory / (name + (arrays ? "-array" : "-standalone") + ".zenasset");
            ASSERT_NO_FATAL_FAILURE(CookPageRenderingPackage(package, compressed, arrays, minification, spatial));
            ASSERT_NO_FATAL_FAILURE(CapturePageRenderingPackage(*device, editor, package, arrays, minification,
                                                                arrays ? packed : reference, spatial));
        }
        ASSERT_EQ(reference.size(), packed.size());
        for (size_t capture = 0; capture < reference.size(); ++capture)
        {
            SCOPED_TRACE(name + " capture " + std::to_string(capture));
            ASSERT_EQ(reference[capture].size(), packed[capture].size());
            size_t   mismatches = 0;
            uint32_t maxError   = 0;
            for (size_t channel = 0; channel < reference[capture].size(); ++channel)
            {
                const uint32_t difference =
                    static_cast<uint32_t>(std::abs(int(reference[capture][channel]) - int(packed[capture][channel])));
                mismatches += difference != 0 ? 1u : 0u;
                maxError    = std::max(maxError, difference);
            }
            EXPECT_EQ(mismatches, 0u) << "maximum channel error " << maxError;
        }
    }
    std::filesystem::remove_all(directory, fileError);
    EXPECT_FALSE(fileError);
}

TEST_P(EditorRendering, CookedArrayPackagesMatchStandaloneMaterialsVariantsAndInstances)
{
    CompareCookedPageScenes(GetParam(), false);
}

TEST_P(EditorRendering, CookedArrayPackagesMatchStandaloneMinificationAndGrazingViews)
{
    CompareCookedPageScenes(GetParam(), true);
}

TEST_P(EditorRendering, CookedSpatialPackagesMatchStandaloneMaterialsAndGrazingViews)
{
    CompareCookedPageScenes(GetParam(), true, true);
}
