TEST_F(RenderCoreTest, AuthoredSceneMipChainsUploadEveryLevelWithoutBlitting)
{
    sg::Scene scene;

    for (uint32_t index = 0; index < 2; ++index)
    {
        UniquePtr<sg::Texture> texture = MakeUnique<sg::Texture>("authored_mips");

        texture->width                 = 8;

        texture->height                = 4;

        texture->format                = index == 0 ? asset::Format::R8G8B8A8_SRGB : asset::Format::R8G8B8A8_UNORM;

        for (uint32_t level = 0; level < 4; ++level)
        {
            const uint32_t width  = std::max(1u, texture->width >> level);

            const uint32_t height = std::max(1u, texture->height >> level);

            texture->mipBytes.emplace_back(width * height * 4, static_cast<uint8_t>(index * 64 + level + 1));
        }

        texture->bytesData.assign(texture->mipBytes[0].begin(), texture->mipBytes[0].end());

        scene.AddComponent(std::move(texture));
    }

    StagingBufferManager staging(4096, 4096);

    StagingUploadQueue uploads(device, &staging);

    StagingAllocation probe;

    ASSERT_EQ(staging.Allocate(16, 16, &probe), StagingFlushAction::eNone);

    TestBuffer* payload = static_cast<TestBuffer*>(probe.pBuffer);

    TextureManager textures(device, &uploads);

    HeapVector<RHITexture*> outputs;

    textures.LoadSceneTextures(&scene, outputs);

    ASSERT_EQ(outputs.size(), 2u);

    EXPECT_EQ(outputs[0]->GetFormat(), DataFormat::eR8G8B8A8SRGB);

    EXPECT_EQ(outputs[1]->GetFormat(), DataFormat::eR8G8B8A8UNORM);

    EXPECT_EQ(outputs[0]->GetNumMipmaps(), 4u);

    EXPECT_EQ(outputs[1]->GetNumMipmaps(), 4u);

    ASSERT_TRUE(uploads.Flush());

    HeapVector<RHIBufferTextureCopyRegion> copies;

    for (const TestContext* context : {&rhi->graphics, &rhi->transfer})
    {
        copies.insert(copies.end(), context->textureCopies.begin(), context->textureCopies.end());

        EXPECT_TRUE(context->blits.empty());
    }

    ASSERT_EQ(copies.size(), 8u);

    for (uint32_t index = 0; index < 2; ++index)
    {
        for (uint32_t level = 0; level < 4; ++level)
        {
            const RHIBufferTextureCopyRegion& region = copies[index * 4 + level];

            const uint32_t width                     = std::max(1u, 8u >> level);

            const uint32_t height                    = std::max(1u, 4u >> level);

            EXPECT_EQ(region.textureSubresources.mipmap, level);

            EXPECT_EQ(region.textureSubresources.baseArrayLayer, 0u);

            EXPECT_EQ(region.textureSubresources.layerCount, 1u);

            EXPECT_EQ(region.textureSize, Vec3i(width, height, 1));

            ASSERT_LE(region.bufferOffset + width * height * 4, payload->bytes.size());

            for (uint32_t byte = 0; byte < width * height * 4; ++byte)
            {
                EXPECT_EQ(payload->bytes[region.bufferOffset + byte], index * 64 + level + 1);
            }
        }
    }

    staging.Release(probe, {});

    textures.Destroy();

    uploads.Destroy();

    staging.Destroy();
}

TEST_F(RenderCoreEnvironmentTest, AuthoredEnvironmentKeepsRoughnessMipsAndOnlyGeneratesBRDFLut)
{
    for (const std::array<const char*, 3>& shader :
         {std::array<const char*, 3>{"SkyboxRenderSP", "SceneRenderer/deferred.vert.spv", "Environment/skybox.frag.spv"},
          {"EnvMapBRDFLutGenSP", "Environment/genbrdflut.vert.spv", "Environment/genbrdflut.frag.spv"}})
    {
        RHIShaderCreateInfo info{};

        info.stageFlags.SetFlags(RHIShaderStageFlagBits::eVertex, RHIShaderStageFlagBits::eFragment);

        info.spirvFileName[ToUnderlying(RHIShaderStage::eVertex)]   = shader[1];

        info.spirvFileName[ToUnderlying(RHIShaderStage::eFragment)] = shader[2];

        reflectedShaderInfos[shader[0]]                             = info;

        CreateTestShaderProgram(device, shader[0]);
    }

    SkyboxRenderer* skybox = device->GetRendererServer()->RequestSkyboxRenderer();

    skybox->Init();

    viewport.color = Texture();

    rc::TextureFormat depth{};

    depth.format = DataFormat::eD32SFloat;

    depth.width = depth.height = 8;

    depth.depth                = 1;

    viewport.depth             = device->CreateTextureDepthStencilRT(depth, {}, "authored_environment_depth");

    sg::Scene source;

    sg::ImageBasedLightAsset light;

    light.size                      = 4;

    light.mipLevels                 = 3;

    light.irradianceCoefficients    = HeapVector<Vec3>(9, Vec3(0));

    light.irradianceCoefficients[0] = Vec3(2.0f);

    for (uint32_t level = 0; level < light.mipLevels; ++level)
    {
        const uint32_t size = std::max(1u, light.size >> level);

        for (uint32_t face = 0; face < 6; ++face)
        {
            light.specularMipFaces.emplace_back(size * size, Vec4(level * 6 + face + 1, 0.25f, 0.5f, 1.0f));
        }
    }

    source.GetAssetData().imageBasedLight = 0;

    source.GetAssetData().imageBasedLights.push_back(std::move(light));

    StagingBufferManager staging(1024 * 1024, 1024 * 1024);

    StagingUploadQueue uploads(device, &staging);

    StagingAllocation probe;

    ASSERT_EQ(staging.Allocate(16, 16, &probe), StagingFlushAction::eNone);

    TestBuffer* payload = static_cast<TestBuffer*>(probe.pBuffer);

    TextureManager textures(device, &uploads);

    EnvTexture environment;

    const uint32_t beforeLoad = rhi->textureCreations;

    textures.LoadSceneEnvironment(&source, &environment);

    ASSERT_TRUE(environment.authoredCubemaps);

    ASSERT_NE(environment.pSkybox, nullptr);

    ASSERT_NE(environment.pIrradiance, nullptr);

    ASSERT_NE(environment.pLutBRDF, nullptr);

    EXPECT_EQ(environment.pPrefiltered, environment.pSkybox);

    EXPECT_EQ(environment.pPrefiltered->GetNumMipmaps(), 3u);

    EXPECT_EQ(environment.pPrefiltered->GetWidth(), 4u);

    EXPECT_EQ(environment.pPrefiltered->GetArrayLayers(), 6u);

    EXPECT_EQ(environment.pIrradiance->GetNumMipmaps(), 1u);

    EXPECT_EQ(environment.pIrradiance->GetWidth(), 64u);

    EXPECT_EQ(environment.pPrefiltered->GetFormat(), DataFormat::eR32G32B32A32SFloat);

    EXPECT_EQ(rhi->textureCreations - beforeLoad, 3u);

    const uint64_t specularId   = environment.pPrefiltered->GetStableId();

    const uint64_t irradianceId = environment.pIrradiance->GetStableId();

    ASSERT_TRUE(uploads.Flush());

    HeapVector<RHIBufferTextureCopyRegion> copies;

    for (const TestContext* context : {&rhi->graphics, &rhi->transfer})
    {
        copies.insert(copies.end(), context->textureCopies.begin(), context->textureCopies.end());

        EXPECT_TRUE(context->blits.empty());
    }

    ASSERT_EQ(copies.size(), 24u);

    for (uint32_t level = 0; level < 3; ++level)
    {
        const uint32_t size = std::max(1u, 4u >> level);

        for (uint32_t face = 0; face < 6; ++face)
        {
            const RHIBufferTextureCopyRegion& region = copies[level * 6 + face];

            EXPECT_EQ(region.textureSubresources.mipmap, level);

            EXPECT_EQ(region.textureSubresources.baseArrayLayer, face);

            EXPECT_EQ(region.textureSubresources.layerCount, 1u);

            EXPECT_EQ(region.textureSize, Vec3i(size, size, 1));

            ASSERT_LE(region.bufferOffset + size * size * sizeof(Vec4), payload->bytes.size());

            for (uint32_t pixel = 0; pixel < size * size; ++pixel)
            {
                Vec4 value;

                std::memcpy(&value, payload->bytes.data() + region.bufferOffset + pixel * sizeof(Vec4), sizeof(value));

                EXPECT_EQ(value, Vec4(level * 6 + face + 1, 0.25f, 0.5f, 1.0f));
            }
        }
    }

    for (uint32_t face = 0; face < 6; ++face)
    {
        const RHIBufferTextureCopyRegion& region = copies[18 + face];

        EXPECT_EQ(region.textureSubresources.mipmap, 0u);

        EXPECT_EQ(region.textureSubresources.baseArrayLayer, face);

        EXPECT_EQ(region.textureSize, Vec3i(64, 64, 1));

        Vec4 value;

        std::memcpy(&value, payload->bytes.data() + region.bufferOffset, sizeof(value));

        EXPECT_NEAR(value.x, 2.0f * 0.2820947918f / glm::pi<float>(), 1e-6f);

        EXPECT_EQ(Vec3(value), Vec3(value.x));

        EXPECT_FLOAT_EQ(value.w, 1.0f);
    }

    sceneInputs.environment = environment;

    SceneData data{};

    data.pScene = &source;

    RenderScene scene(device, data);

    skybox->SetRenderScene(&scene);

    RenderGraph& graph = *device->GetCurrentFrameRDG();

    ASSERT_TRUE(graph.Begin());

    skybox->BuildRenderGraph(RenderView::FromViewport(viewport));

    ASSERT_TRUE(graph.End());

    ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;

    skybox->OnRenderGraphExecuted(true);

    EXPECT_EQ(rhi->graphics.renderingLayouts.size(), 2u);

    EXPECT_EQ(rhi->graphics.drawCount, 2u);

    EXPECT_TRUE(rhi->graphics.indexedDraws.empty());

    EXPECT_TRUE(rhi->graphics.imageCopies.empty());

    EXPECT_TRUE(rhi->transfer.imageCopies.empty());

    EXPECT_TRUE(rhi->graphics.blits.empty());

    EXPECT_TRUE(rhi->transfer.blits.empty());

    EXPECT_EQ(environment.pPrefiltered->GetStableId(), specularId);

    EXPECT_EQ(environment.pIrradiance->GetStableId(), irradianceId);

    staging.Release(probe, {});

    textures.Destroy();

    uploads.Destroy();

    staging.Destroy();

    device->DestroyTexture(viewport.color);

    device->DestroyTexture(viewport.depth);

    sceneInputs = {};
}
