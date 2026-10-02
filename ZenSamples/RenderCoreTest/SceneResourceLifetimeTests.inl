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

    HeapVector<RHITexture*> first;

    HeapVector<RHITexture*> second;

    textures.LoadSceneTextures(&source, first);

    textures.LoadSceneTextures(&source, second);

    ASSERT_EQ(first.size(), 1u);

    ASSERT_EQ(second.size(), 1u);

    const uint64_t cachedId = cached->GetStableId();

    const uint64_t firstId  = first[0]->GetStableId();

    const uint64_t secondId = second[0]->GetStableId();

    ASSERT_TRUE(uploads.Flush());

    EXPECT_FALSE(textures.ReleaseSceneTexture(cached));

    EXPECT_TRUE(textures.ReleaseSceneTexture(first[0]));

    EXPECT_FALSE(textures.ReleaseSceneTexture(first[0]));

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

    HeapVector<RHITexture*> outputs;

    const size_t before         = rhi->createdTextureIds.size();

    rhi->throwTextureCreationAt = rhi->textureCreations + 2;

    EXPECT_THROW(device->LoadSceneTextures(&source, outputs), std::runtime_error);

    ASSERT_EQ(outputs.size(), 1u);

    ASSERT_EQ(rhi->createdTextureIds.size(), before + 1);

    const uint64_t id = outputs[0]->GetStableId();

    EXPECT_TRUE(device->ReleaseSceneTexture(outputs[0]));

    ASSERT_TRUE(device->PrepareForResourceReconfiguration());

    EXPECT_TRUE(destroyed.contains(id));
}

TEST_F(RenderCoreTest, InitialBufferUploadAllocationFailureRetiresUnpublishedDestination)
{
    const std::array<uint8_t, 4> payload = {1, 2, 3, 4};

    for (uint32_t kind = 0; kind < 3; ++kind)
    {
        // Destination creation succeeds; allocating staging for its initial upload throws.
        rhi->throwBufferCreationAt = rhi->bufferCreations + 2;

        switch (kind)
        {
            case 0: EXPECT_THROW(device->CreateVertexBuffer(payload.size(), payload.data()), std::runtime_error); break;

            case 1: EXPECT_THROW(device->CreateIndexBuffer(payload.size(), payload.data()), std::runtime_error); break;

            case 2:
                EXPECT_THROW(device->CreateStorageBuffer(payload.size(), payload.data(), "failed_scene_upload"),
                             std::runtime_error);
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

    const size_t before         = rhi->createdTextureIds.size();

    rhi->throwTextureCreationAt = rhi->textureCreations + 3;

    EXPECT_THROW(device->LoadSceneEnvironment(&source, &environment), std::runtime_error);

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

    RHIBuffer** slots[] = {&sceneInputs.vertices, &sceneInputs.indices, &sceneInputs.uv, &sceneInputs.nodes,
                           &sceneInputs.materials};

    HeapVector<uint64_t> ids;

    for (RHIBuffer** slot : slots)
    {
        *slot = device->CreateStorageBuffer(64, nullptr, "scene_lifetime_buffer");

        ids.push_back((*slot)->GetStableId());
    }

    device->LoadSceneTextures(&source, sceneInputs.textures);

    ASSERT_EQ(sceneInputs.textures.size(), 1u);

    ids.push_back(sceneInputs.textures[0]->GetStableId());

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

    EXPECT_EQ(scene.GetVertexBuffer(), nullptr);

    EXPECT_EQ(scene.GetIndexBuffer(), nullptr);

    EXPECT_EQ(scene.GetUVBuffer(), nullptr);

    EXPECT_EQ(scene.GetNodesDataSSBO(), nullptr);

    EXPECT_EQ(scene.GetMaterialsDataSSBO(), nullptr);

    EXPECT_EQ(scene.GetVoxelTriangleBuffer(), nullptr);

    EXPECT_TRUE(scene.GetSceneTextures().empty());

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

    sceneInputs.nodes = device->CreateStorageBuffer(64, nullptr, "partial_scene_nodes");

    const uint64_t id = sceneInputs.nodes->GetStableId();

    // Exercise duplicate and absent handles from a failed initialization.
    sceneInputs.materials = sceneInputs.nodes;

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

    skybox->BuildRenderGraph();

    ASSERT_TRUE(graph.End());

    ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;

    EXPECT_EQ(rhi->graphics.drawCount, 2u);

    // A rejected old frame must not requeue preprocessing after its scene is retired.
    skybox->CancelEnvironmentPreprocessing(&sceneInputs.environment);

    skybox->OnRenderGraphExecuted(false);

    ASSERT_TRUE(device->PrepareForResourceReconfiguration());

    const uint32_t before = rhi->graphics.drawCount;

    ASSERT_TRUE(graph.Begin());

    skybox->BuildRenderGraph();

    ASSERT_TRUE(graph.End());

    ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;

    EXPECT_EQ(rhi->graphics.drawCount - before, 1u);

    skybox->OnRenderGraphExecuted(true);

    EnvTexture abandoned;

    device->LoadSceneEnvironment(&source, &abandoned);

    device->ReleaseSceneEnvironment(&abandoned);

    ASSERT_TRUE(device->PrepareForResourceReconfiguration());

    const uint32_t after = rhi->graphics.drawCount;

    ASSERT_TRUE(graph.Begin());

    skybox->BuildRenderGraph();

    ASSERT_TRUE(graph.End());

    ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;

    EXPECT_EQ(rhi->graphics.drawCount - after, 1u);

    skybox->OnRenderGraphExecuted(true);

    ASSERT_TRUE(device->PrepareForResourceReconfiguration());

    skybox->SetRenderScene(nullptr);

    scene.Destroy();

    device->DestroyTexture(viewport.color);

    device->DestroyTexture(viewport.depth);

    sceneInputs = {};
}
