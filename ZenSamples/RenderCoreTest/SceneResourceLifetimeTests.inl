static void AddLifetimeTexture(sg::Scene& scene)
{
    UniquePtr<sg::Texture> texture = MakeUnique<sg::Texture>("scene_lifetime");

    texture->width = texture->height = 2;

    texture->format                  = asset::Format::R8G8B8A8_UNORM;

    texture->bytesData.resize(16, 127);

    scene.AddComponent(std::move(texture));
}

static void AddLifetimeEnvironment(sg::Scene& scene)
{
    sg::ImageBasedLightAsset light;

    light.size                   = 1;

    light.mipLevels              = 1;

    light.irradianceCoefficients = HeapVector<Vec3>(9, Vec3(0));

    for (uint32_t face = 0; face < 6; ++face)
    {
        light.specularMipFaces.emplace_back(1, Vec4(0.25f, 0.5f, 0.75f, 1));
    }

    scene.GetAssetData().imageBasedLight = 0;

    scene.GetAssetData().imageBasedLights.push_back(std::move(light));
}

static void PrepareLifetimeSkybox(RenderDevice* device)
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

    device->GetRendererServer()->RequestSkyboxRenderer()->Init();
}

TEST_F(RenderCoreEnvironmentTest, BC6HEnvironmentUploadsAuthoredCubeBlocksAndKeepsFloatIrradiance)
{
    PrepareLifetimeSkybox(device);
    sg::Scene source;
    AddLifetimeEnvironment(source);
    sg::ImageBasedLightAsset& light = source.GetAssetData().imageBasedLights[0];
    light.size                      = 5;
    light.mipLevels                 = 3;
    light.specularFormat            = asset::Format::BC6H_UFLOAT_BLOCK;
    light.specularMipFaces.clear();
    light.irradianceCoefficients[0] = Vec3(2);
    SharedPtr<HeapVector<HeapVector<uint8_t>>, MultiThreadCounter> blocks =
        MakeShared<HeapVector<HeapVector<uint8_t>>, MultiThreadCounter>();
    for (uint32_t face = 0; face < 18; ++face)
    {
        blocks->emplace_back(face < 6 ? 64 : 16, static_cast<uint8_t>(face + 1));
    }
    light.specularBlockMipFaces = blocks;
    StagingBufferManager staging(1024 * 1024, 1024 * 1024);
    StagingUploadQueue   uploads(device, &staging);
    StagingAllocation    probe;
    ASSERT_EQ(staging.Allocate(16, 16, &probe), StagingFlushAction::eNone);
    TestBuffer*    payload = static_cast<TestBuffer*>(probe.pBuffer);
    TextureManager textures(device, &uploads);
    EnvTexture     environment;
    textures.LoadSceneEnvironment(&source, &environment);
    ASSERT_TRUE(environment.IsComplete());
    EXPECT_TRUE(environment.authoredCubemaps);
    EXPECT_EQ(environment.pPrefiltered, environment.pSkybox);
    EXPECT_EQ(environment.pSkybox->GetFormat(), DataFormat::eBC6HUFloatBlock);
    EXPECT_EQ(environment.pSkybox->GetWidth(), 5u);
    EXPECT_EQ(environment.pSkybox->GetNumMipmaps(), 3u);
    EXPECT_EQ(environment.pSkybox->GetArrayLayers(), 6u);
    EXPECT_EQ(environment.pIrradiance->GetFormat(), DataFormat::eR32G32B32A32SFloat);
    ASSERT_TRUE(uploads.Flush());
    HeapVector<RHIBufferTextureCopyRegion> copies;
    for (const TestContext* context : {&rhi->graphics, &rhi->transfer})
    {
        copies.insert(copies.end(), context->textureCopies.begin(), context->textureCopies.end());
        EXPECT_TRUE(context->blits.empty());
    }
    ASSERT_EQ(copies.size(), 24u);
    for (uint32_t face = 0; face < 18; ++face)
    {
        const RHIBufferTextureCopyRegion& region = copies[face];
        const uint32_t                    level  = face / 6;
        const uint32_t                    extent = std::max(1u, 5u >> level);
        const uint32_t                    bytes  = level == 0 ? 64 : 16;
        EXPECT_EQ(region.textureSubresources.mipmap, level);
        EXPECT_EQ(region.textureSubresources.baseArrayLayer, face % 6);
        EXPECT_EQ(region.textureSubresources.layerCount, 1u);
        EXPECT_EQ(region.textureSize, Vec3i(extent, extent, 1));
        EXPECT_EQ(region.bufferOffset % 16, 0u);
        ASSERT_LE(region.bufferOffset + bytes, payload->bytes.size());
        for (uint32_t byte = 0; byte < bytes; ++byte)
        {
            EXPECT_EQ(payload->bytes[region.bufferOffset + byte], face + 1);
        }
    }
    Vec4 irradiance;
    std::memcpy(&irradiance, payload->bytes.data() + copies[18].bufferOffset, sizeof(irradiance));
    EXPECT_NEAR(irradiance.x, 2.0f * 0.2820947918f / glm::pi<float>(), 1e-6f);
    textures.ReleaseSceneEnvironment(&environment);
    staging.Release(probe, {});
    textures.Destroy();
    uploads.Destroy();
    staging.Destroy();
}

TEST_F(RenderCoreEnvironmentTest, BC6HEnvironmentRejectsMalformedAndUnsupportedCubesWithoutPublication)
{
    for (uint32_t scenario = 0; scenario < 5; ++scenario)
    {
        sg::Scene source;
        AddLifetimeEnvironment(source);
        sg::ImageBasedLightAsset& light = source.GetAssetData().imageBasedLights[0];
        light.specularFormat            = asset::Format::BC6H_UFLOAT_BLOCK;
        light.specularMipFaces.clear();
        SharedPtr<HeapVector<HeapVector<uint8_t>>, MultiThreadCounter> blocks =
            MakeShared<HeapVector<HeapVector<uint8_t>>, MultiThreadCounter>();
        *blocks                     = HeapVector<HeapVector<uint8_t>>(6, HeapVector<uint8_t>(16, 0));
        light.specularBlockMipFaces = blocks;
        if (scenario == 0)
        {
            blocks->back().pop_back();
        }
        else if (scenario == 1)
        {
            blocks->pop_back();
        }
        else if (scenario == 2)
        {
            light.specularMipFaces.emplace_back(1, Vec4(1));
        }
        else if (scenario == 3)
        {
            light.mipLevels = 0;
        }
        else
        {
            rhi->copyCapabilities[DataFormat::eBC6HUFloatBlock] = {};
        }
        StagingBufferManager staging(4096, 4096);
        StagingUploadQueue   uploads(device, &staging);
        TextureManager       textures(device, &uploads);
        EnvTexture           environment;
        const uint32_t       before = rhi->textureCreations;
        textures.LoadSceneEnvironment(&source, &environment);
        EXPECT_FALSE(environment.IsComplete());
        EXPECT_EQ(environment.pSkybox, nullptr);
        EXPECT_EQ(environment.pIrradiance, nullptr);
        EXPECT_FALSE(uploads.HasPending());
        EXPECT_EQ(rhi->textureCreations, before + (scenario == 4 ? 1u : 0u));
        textures.Destroy();
        uploads.Destroy();
        staging.Destroy();
        rhi->copyCapabilities.clear();
    }
}

TEST_F(RenderCoreTest, SceneTextureReleaseKeepsCachedAssetsAndOtherScenesAlive)
{
    asset::TextureInfo input{};

    input.width = input.height = 2;

    input.data.resize(16, 31);

    textureFiles["shared_lifetime.png"] = input;

    sg::Scene source;

    AddLifetimeTexture(source);

    StagingBufferManager staging(1024, 4096);

    StagingUploadQueue uploads(device, &staging);

    TextureManager textures(device, &uploads);

    RHITexture* cached = textures.LoadTexture2D("shared_lifetime.png");

    ASSERT_NE(cached, nullptr);

    HeapVector<RHITextureView*> first;

    HeapVector<RHITextureView*> second;

    textures.LoadSceneTextures(&source, first);

    textures.LoadSceneTextures(&source, second);

    ASSERT_EQ(first.size(), 1u);

    ASSERT_EQ(second.size(), 1u);

    const uint64_t cachedId = cached->GetStableId();

    const uint64_t firstId  = first[0]->GetTexture()->GetStableId();

    const uint64_t secondId = second[0]->GetTexture()->GetStableId();

    ASSERT_TRUE(uploads.Flush());

    EXPECT_FALSE(textures.ReleaseSceneTexture(cached));

    EXPECT_TRUE(textures.ReleaseSceneTexture(first[0]->GetTexture()));

    EXPECT_FALSE(textures.ReleaseSceneTexture(first[0]->GetTexture()));

    EXPECT_FALSE(destroyed.contains(firstId));

    rhi->completed = rhi->submitted;

    uploads.ReclaimResources();

    device->CollectCompletedResources();

    EXPECT_TRUE(destroyed.contains(firstId));

    EXPECT_FALSE(destroyed.contains(cachedId));

    EXPECT_FALSE(destroyed.contains(secondId));

    EXPECT_EQ(textures.LoadTexture2D("./shared_lifetime.png"), cached);

    textures.Destroy();

    uploads.Destroy();

    staging.Destroy();

    rhi->completed = rhi->submitted;

    device->CollectCompletedResources();

    EXPECT_TRUE(destroyed.contains(cachedId));

    EXPECT_TRUE(destroyed.contains(secondId));
}

TEST_F(RenderCoreTest, PartialSceneTextureAllocationPublishesCleanupHandles)
{
    sg::Scene source;

    AddLifetimeTexture(source);

    AddLifetimeTexture(source);

    HeapVector<RHITextureView*> outputs;

    const size_t before        = rhi->createdTextureIds.size();

    rhi->failTextureCreationAt = rhi->textureCreations + 2;

    device->LoadSceneTextures(&source, outputs);

    ASSERT_EQ(outputs.size(), 2u);

    EXPECT_EQ(outputs[1], nullptr);

    ASSERT_EQ(rhi->createdTextureIds.size(), before + 1);

    const uint64_t id = outputs[0]->GetTexture()->GetStableId();

    EXPECT_TRUE(device->ReleaseSceneTexture(outputs[0]->GetTexture()));

    ASSERT_TRUE(device->PrepareForResourceReconfiguration());

    EXPECT_TRUE(destroyed.contains(id));
}

TEST_F(RenderCoreTest, FailedTextureStagingRetiresUnpublishedDestination)
{
    sg::Scene source;

    AddLifetimeTexture(source);

    StagingBufferManager staging(1024, 4096);

    StagingUploadQueue uploads(device, &staging);

    TextureManager textures(device, &uploads);

    rhi->failBufferCreationAt = rhi->bufferCreations + 1;

    HeapVector<RHITextureView*> outputs;

    textures.LoadSceneTextures(&source, outputs);

    ASSERT_EQ(outputs.size(), 1u);

    EXPECT_EQ(outputs[0], nullptr);

    EXPECT_FALSE(uploads.HasPending());

    const uint64_t id = rhi->createdTextureIds.back();

    EXPECT_TRUE(device->PrepareForResourceReconfiguration());

    EXPECT_TRUE(destroyed.contains(id));

    textures.Destroy();

    uploads.Destroy();

    staging.Destroy();
}

TEST_F(RenderCoreEnvironmentTest, SceneReadinessRejectsMissingBuffersTexturesAndSamplers)
{
    sg::Scene source;

    AddLifetimeTexture(source);

    AddLifetimeEnvironment(source);

    source.AddComponent(MakeUnique<sg::Sampler>("readiness_sampler"));

    RHIBuffer** buffers[] = {&sceneInputs.vertices, &sceneInputs.indices, &sceneInputs.uv, &sceneInputs.nodes,
                             &sceneInputs.materials};

    for (RHIBuffer** buffer : buffers)
    {
        *buffer = device->CreateStorageBuffer(64, nullptr, "readiness_buffer");
    }

    device->LoadSceneTextures(&source, sceneInputs.textures);

    device->LoadSceneEnvironment(&source, &sceneInputs.environment);

    sceneInputs.samplers.push_back(device->CreateSampler(RHISamplerCreateInfo::CreateLinearRepeat()));

    SceneData data{};

    data.pScene = &source;

    // The fixture injects handles into RenderScene; readiness uses production code.
    RenderScene complete(device, data);

    ASSERT_TRUE(complete.HasRequiredResources());

    for (RHIBuffer** buffer : buffers)
    {
        RHIBuffer* saved = *buffer;

        *buffer          = nullptr;

        RenderScene partial(device, data);

        EXPECT_FALSE(partial.HasRequiredResources());

        *buffer = saved;
    }

    RHITextureView* savedView = sceneInputs.textures[0];
    sceneInputs.textures[0]   = nullptr;
    RenderScene missingView(device, data);
    EXPECT_FALSE(missingView.HasRequiredResources());
    sceneInputs.textures[0] = savedView;

    RHITexture** textures[] = {&sceneInputs.environment.pSkybox, &sceneInputs.environment.pIrradiance,
                               &sceneInputs.environment.pPrefiltered, &sceneInputs.environment.pLutBRDF};

    for (RHITexture** texture : textures)
    {
        RHITexture* saved = *texture;

        *texture          = nullptr;

        RenderScene partial(device, data);

        EXPECT_FALSE(partial.HasRequiredResources());

        *texture = saved;
    }

    RHISampler** samplers[] = {&sceneInputs.samplers[0], &sceneInputs.environment.pIrradianceSampler,
                               &sceneInputs.environment.pPrefilteredSampler, &sceneInputs.environment.pLutBRDFSampler};

    for (RHISampler** sampler : samplers)
    {
        RHISampler* saved = *sampler;

        *sampler          = nullptr;

        RenderScene partial(device, data);

        EXPECT_FALSE(partial.HasRequiredResources());

        *sampler = saved;
    }

    device->GetRendererServer()->RequestSkyboxRenderer()->CancelEnvironmentPreprocessing(&sceneInputs.environment);

    EXPECT_TRUE(device->PrepareForResourceReconfiguration());

    complete.Destroy();

    // The facade only hands the index buffer to the scene; it owns the ring-backed inputs.
    for (RHIBuffer* buffer : {sceneInputs.vertices, sceneInputs.uv, sceneInputs.nodes, sceneInputs.materials})
    {
        device->DestroyBuffer(buffer);
    }

    sceneInputs = {};
}

TEST_F(RenderCoreTest, InitialBufferUploadAllocationFailureRetiresUnpublishedDestination)
{
    const std::array<uint8_t, 4> payload = {1, 2, 3, 4};

    for (uint32_t kind = 0; kind < 3; ++kind)
    {
        // Destination creation succeeds; allocating staging for its initial upload returns null.
        rhi->failBufferCreationAt = rhi->bufferCreations + 2;

        switch (kind)
        {
            case 0: EXPECT_EQ(device->CreateVertexBuffer(payload.size(), payload.data()), nullptr); break;

            case 1: EXPECT_EQ(device->CreateIndexBuffer(payload.size(), payload.data()), nullptr); break;

            case 2:
                EXPECT_EQ(device->CreateStorageBuffer(payload.size(), payload.data(), "failed_scene_upload"), nullptr);
                break;

            default: break;
        }

        const uint64_t destination = rhi->lastBufferId;

        ASSERT_TRUE(device->PrepareForResourceReconfiguration());

        EXPECT_TRUE(destroyed.contains(destination));
    }
}

TEST_F(RenderCoreEnvironmentTest, PartialEnvironmentAllocationRetiresPublishedCubemaps)
{
    sg::Scene source;

    AddLifetimeEnvironment(source);

    EnvTexture environment;

    const size_t before        = rhi->createdTextureIds.size();

    rhi->failTextureCreationAt = rhi->textureCreations + 3;

    device->LoadSceneEnvironment(&source, &environment);

    ASSERT_NE(environment.pSkybox, nullptr);

    ASSERT_NE(environment.pIrradiance, nullptr);

    EXPECT_EQ(environment.pPrefiltered, environment.pSkybox);

    EXPECT_EQ(environment.pLutBRDF, nullptr);

    ASSERT_EQ(rhi->createdTextureIds.size(), before + 2);

    const uint64_t specularId   = environment.pSkybox->GetStableId();

    const uint64_t irradianceId = environment.pIrradiance->GetStableId();

    device->ReleaseSceneEnvironment(&environment);

    device->ReleaseSceneEnvironment(&environment);

    EXPECT_EQ(environment.pSkybox, nullptr);

    EXPECT_EQ(environment.pIrradiance, nullptr);

    EXPECT_EQ(environment.pPrefiltered, nullptr);

    ASSERT_TRUE(device->PrepareForResourceReconfiguration());

    EXPECT_TRUE(destroyed.contains(specularId));

    EXPECT_TRUE(destroyed.contains(irradianceId));
}

TEST_F(RenderCoreEnvironmentTest, ProductionSceneDestroyRetiresBuffersAndAliasedEnvironmentOnce)
{
    sg::Scene source;

    AddLifetimeTexture(source);

    AddLifetimeEnvironment(source);

    // Ring-backed scene storage is covered by the FrameBufferRing tests; the facade
    // hands only the index buffer to the scene.
    HeapVector<uint64_t> ids;

    sceneInputs.indices = device->CreateStorageBuffer(64, nullptr, "scene_lifetime_buffer");

    ids.push_back(sceneInputs.indices->GetStableId());

    device->LoadSceneTextures(&source, sceneInputs.textures);

    ASSERT_EQ(sceneInputs.textures.size(), 1u);

    ids.push_back(sceneInputs.textures[0]->GetTexture()->GetStableId());

    sceneInputs.textures.push_back(sceneInputs.textures[0]);

    device->LoadSceneEnvironment(&source, &sceneInputs.environment);

    device->GetRendererServer()->RequestSkyboxRenderer()->CancelEnvironmentPreprocessing(&sceneInputs.environment);

    ASSERT_EQ(sceneInputs.environment.pSkybox, sceneInputs.environment.pPrefiltered);

    for (RHITexture* texture :
         {sceneInputs.environment.pSkybox, sceneInputs.environment.pIrradiance, sceneInputs.environment.pLutBRDF})
    {
        ASSERT_NE(texture, nullptr);

        ids.push_back(texture->GetStableId());
    }

    SceneData data{};

    data.pScene = &source;

    RenderScene scene(device, data);

    ASSERT_TRUE(device->PrepareForResourceReconfiguration());

    scene.Destroy();

    scene.Destroy();

    EXPECT_EQ(scene.GetIndexBuffer(), nullptr);

    EXPECT_EQ(scene.GetVoxelTriangleBuffer(), nullptr);

    EXPECT_TRUE(scene.GetSceneTextureViews().empty());

    EXPECT_EQ(scene.GetEnvTexture().pSkybox, nullptr);

    EXPECT_EQ(scene.GetEnvTexture().pPrefiltered, nullptr);

    ASSERT_TRUE(device->PrepareForResourceReconfiguration());

    for (uint64_t id : ids)
    {
        EXPECT_TRUE(destroyed.contains(id));
    }

    sceneInputs = {};
}

TEST_F(RenderCoreTest, ProductionSceneDestroyAcceptsPartiallyInitializedAndEmptyScenes)
{
    sg::Scene source;

    // A failed initialization can leave the index buffer without its other scene buffers.
    sceneInputs.indices = device->CreateStorageBuffer(64, nullptr, "partial_scene_indices");

    const uint64_t id   = sceneInputs.indices->GetStableId();

    SceneData data{};

    data.pScene = &source;

    RenderScene partial(device, data);

    partial.Destroy();

    partial.Destroy();

    ASSERT_TRUE(device->PrepareForResourceReconfiguration());

    EXPECT_TRUE(destroyed.contains(id));

    sceneInputs = {};

    RenderScene empty(device, data);

    empty.Destroy();

    empty.Destroy();

    EXPECT_EQ(empty.GetNodesDataSSBO(), nullptr);
}

TEST_F(RenderCoreEnvironmentTest, EnvironmentCancellationPreservesNewSceneAndClearsRejectedWork)
{
    PrepareLifetimeSkybox(device);

    viewport.color = Texture();

    zen::rc::TextureFormat depth{};

    depth.format = DataFormat::eD32SFloat;

    depth.width = depth.height = 8;

    depth.depth                = 1;

    viewport.depth             = device->CreateTextureDepthStencilRT(depth, {}, "scene_lifetime_depth");

    sg::Scene source;

    AddLifetimeEnvironment(source);

    EnvTexture old;

    device->LoadSceneEnvironment(&source, &old);

    device->LoadSceneEnvironment(&source, &sceneInputs.environment);

    device->ReleaseSceneEnvironment(&old);

    SceneData data{};

    data.pScene = &source;

    RenderScene scene(device, data);

    SkyboxRenderer* skybox = device->GetRendererServer()->RequestSkyboxRenderer();

    skybox->SetRenderScene(&scene);

    RenderGraph& graph = *device->GetCurrentFrameRDG();

    ASSERT_TRUE(graph.Begin());

    skybox->BuildRenderGraph(RenderView::FromViewport(viewport));

    ASSERT_TRUE(graph.End());

    ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;

    EXPECT_EQ(rhi->graphics.drawCount, 2u);

    // A rejected old frame must not requeue preprocessing after its scene is retired.
    skybox->CancelEnvironmentPreprocessing(&sceneInputs.environment);

    skybox->OnRenderGraphExecuted(RenderFrameResult::eFailed);

    ASSERT_TRUE(device->PrepareForResourceReconfiguration());

    const uint32_t before = rhi->graphics.drawCount;

    ASSERT_TRUE(graph.Begin());

    skybox->BuildRenderGraph(RenderView::FromViewport(viewport));

    ASSERT_TRUE(graph.End());

    ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;

    EXPECT_EQ(rhi->graphics.drawCount - before, 1u);

    skybox->OnRenderGraphExecuted(RenderFrameResult::eSubmitted);

    EnvTexture abandoned;

    device->LoadSceneEnvironment(&source, &abandoned);

    device->ReleaseSceneEnvironment(&abandoned);

    ASSERT_TRUE(device->PrepareForResourceReconfiguration());

    const uint32_t after = rhi->graphics.drawCount;

    ASSERT_TRUE(graph.Begin());

    skybox->BuildRenderGraph(RenderView::FromViewport(viewport));

    ASSERT_TRUE(graph.End());

    ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;

    EXPECT_EQ(rhi->graphics.drawCount - after, 1u);

    skybox->OnRenderGraphExecuted(RenderFrameResult::eSubmitted);

    ASSERT_TRUE(device->PrepareForResourceReconfiguration());

    skybox->SetRenderScene(nullptr);

    scene.Destroy();

    device->DestroyTexture(viewport.color);

    device->DestroyTexture(viewport.depth);

    sceneInputs = {};
}
