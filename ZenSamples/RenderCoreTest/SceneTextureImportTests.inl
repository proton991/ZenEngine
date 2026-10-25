TEST_F(RenderCoreTest, BlockSceneMipsUploadWholeBlocksWithoutBlitting)
{
    for (DataFormat format : {DataFormat::eBC4UNORMBlock, DataFormat::eBC5UNORMBlock, DataFormat::eBC7SRGBBlock})
    {
        for (const bool authoredMips : {false, true})
        {
            sg::Scene              scene;
            UniquePtr<sg::Texture> source = MakeUnique<sg::Texture>("bc7_mips");
            source->width                 = 5;
            source->height                = 7;
            source->format                = static_cast<asset::Format>(format);
            source->interpretation    = format == DataFormat::eBC5UNORMBlock ? asset::TextureInterpretation::NormalXYPositiveZ
                                                                             : asset::TextureInterpretation::Default;
            const uint32_t blockBytes = format == DataFormat::eBC4UNORMBlock ? 8u : 16u;
            source->bytesData.resize(4 * blockBytes, 0x40);
            if (authoredMips)
            {
                source->mipBytes = {HeapVector<uint8_t>(source->bytesData.data(), source->bytesData.size()),
                                    HeapVector<uint8_t>(blockBytes, 0x41), HeapVector<uint8_t>(blockBytes, 0x42)};
            }
            scene.AddComponent(std::move(source));
            StagingBufferManager staging(4096, 4096);
            StagingUploadQueue   uploads(device, &staging);
            StagingAllocation    probe;
            ASSERT_EQ(staging.Allocate(16, 16, &probe), StagingFlushAction::eNone);
            TestBuffer*                 payload = static_cast<TestBuffer*>(probe.pBuffer);
            TextureManager              textures(device, &uploads);
            HeapVector<RHITextureView*> outputs;
            textures.LoadSceneTextures(&scene, outputs);
            ASSERT_EQ(outputs.size(), 1u);
            ASSERT_NE(outputs[0], nullptr);
            EXPECT_EQ(outputs[0]->GetFormat(), format);
            EXPECT_EQ(outputs[0]->GetTexture()->GetNumMipmaps(), authoredMips ? 3u : 1u);
            ASSERT_TRUE(uploads.Flush());
            HeapVector<RHIBufferTextureCopyRegion> copies;
            for (TestContext* context : {&rhi->graphics, &rhi->transfer})
            {
                copies.insert(copies.end(), context->textureCopies.begin(), context->textureCopies.end());
                EXPECT_TRUE(context->blits.empty());
                context->textureCopies.clear();
            }
            ASSERT_EQ(copies.size(), authoredMips ? 3u : 1u);
            for (uint32_t level = 0; level < copies.size(); ++level)
            {
                const RHIBufferTextureCopyRegion& copy = copies[level];
                EXPECT_EQ(copy.textureSubresources.mipmap, level);
                EXPECT_EQ(copy.textureSize, Vec3i(std::max(1u, 5u >> level), std::max(1u, 7u >> level), 1));
                EXPECT_EQ(copy.bufferOffset % blockBytes, 0u);
                const uint32_t bytes = level == 0 ? 4 * blockBytes : blockBytes;
                ASSERT_LE(copy.bufferOffset + bytes, payload->bytes.size());
                for (uint32_t byte = 0; byte < bytes; ++byte)
                {
                    EXPECT_EQ(payload->bytes[copy.bufferOffset + byte], 0x40 + level);
                }
            }
            staging.Release(probe, {});
            textures.Destroy();
            uploads.Destroy();
            staging.Destroy();
        }
    }
}

void AddArraySceneTextures(sg::Scene& scene, DataFormat format)
{
    SharedPtr<asset::ImagePixels, MultiThreadCounter> pixels = MakeShared<asset::ImagePixels, MultiThreadCounter>();
    pixels->width                                            = 5;
    pixels->height                                           = 7;
    pixels->arrayLayers                                      = 2;
    pixels->format                                           = static_cast<asset::Format>(format);
    for (uint32_t level = 0; level < 3; ++level)
    {
        uint64_t bytes = 0;
        ASSERT_TRUE(GetTextureFormatLevelSize(format, std::max(1u, 5u >> level), std::max(1u, 7u >> level), 1, bytes));
        pixels->mips.emplace_back(static_cast<size_t>(bytes) * 2, uint8_t(20 + level));
        std::fill(pixels->mips.back().begin() + bytes, pixels->mips.back().end(), uint8_t(40 + level));
    }
    pixels->bytes = pixels->mips[0];
    for (uint32_t index = 0; index < 3; ++index)
    {
        UniquePtr<sg::Texture> source = MakeUnique<sg::Texture>("array_page");
        source->width                 = 5;
        source->height                = 7;
        source->format                = pixels->format;
        if (index == 0 && (format == DataFormat::eR8G8B8A8UNORM || format == DataFormat::eBC7UNORMBlock))
        {
            source->format =
                format == DataFormat::eR8G8B8A8UNORM ? asset::Format::R8G8B8A8_SRGB : asset::Format::BC7_SRGB_BLOCK;
        }
        source->interpretation = format == DataFormat::eBC5UNORMBlock ? asset::TextureInterpretation::NormalXYPositiveZ
                                                                      : asset::TextureInterpretation::Default;
        source->pixels         = pixels;
        source->arrayLayer     = index == 0 ? 0 : 1;
        scene.AddComponent(std::move(source));
    }
}

TEST_F(RenderCoreTest, SpatialRegionsRequireReportedPrecisionBeforeNativeAllocation)
{
    sg::Scene                                         scene;
    SharedPtr<asset::ImagePixels, MultiThreadCounter> pixels = MakeShared<asset::ImagePixels, MultiThreadCounter>();
    pixels->width = pixels->height = 16;
    pixels->bytes                  = HeapVector<uint8_t>(16 * 16 * 4, 127);
    pixels->mips.push_back(pixels->bytes);
    UniquePtr<sg::Texture> texture = MakeUnique<sg::Texture>("Spatial tile");
    texture->width = texture->height = 16;
    texture->format                  = asset::Format::R8G8B8A8_UNORM;
    texture->pixels                  = pixels;
    texture->atlasRegion             = {4, 4, 4, 4};
    sg::Texture* source              = texture.Get();
    scene.AddComponent(std::move(texture));
    RHIGPUInfo gpu;
    gpu.maxStorageBufferRange  = 4096;
    gpu.storageBufferAlignment = 256;
    EXPECT_NE(RenderScene::GetResourceError(gpu, scene, 3, 3).find("spatial region"), std::string::npos);
    StagingBufferManager        staging(4096, 4096);
    StagingUploadQueue          uploads(device, &staging);
    TextureManager              textures(device, &uploads);
    const uint32_t              beforeTextures = rhi->textureCreations;
    const uint32_t              beforeBuffers  = rhi->bufferCreations;
    HeapVector<RHITextureView*> views;
    textures.LoadSceneTextures(&scene, views);
    ASSERT_EQ(views.size(), 1u);
    EXPECT_EQ(views[0], nullptr);
    EXPECT_EQ(rhi->textureCreations, beforeTextures);
    EXPECT_EQ(rhi->bufferCreations, beforeBuffers);
    EXPECT_TRUE(uploads.Flush());
    EXPECT_TRUE(rhi->transfer.imageCopies.empty());
    EXPECT_TRUE(rhi->graphics.imageCopies.empty());
    source->bytesData.resize(16 * 16 * 4, 200);
    EXPECT_TRUE(RenderScene::GetResourceError(gpu, scene, 3, 3).empty());
    views.clear();
    textures.LoadSceneTextures(&scene, views);
    ASSERT_EQ(views.size(), 1u);
    EXPECT_NE(views[0], nullptr);
    textures.Destroy();
    uploads.Destroy();
    staging.Destroy();
}

void AddSpatialSceneTextures(sg::Scene& scene)
{
    UniquePtr<sg::Sampler> sampler = MakeUnique<sg::Sampler>("linear_clamp");
    sampler->minFilter = sampler->magFilter = sampler->mipFilter = asset::TextureFilter::Linear;
    sampler->wrapS = sampler->wrapT = asset::SamplerAddressMode::ClampToEdge;
    scene.AddComponent(std::move(sampler));
    SharedPtr<asset::ImagePixels, MultiThreadCounter> pixels = MakeShared<asset::ImagePixels, MultiThreadCounter>();
    pixels->width = pixels->height = 16;
    pixels->format                 = asset::Format::R8G8B8A8_UNORM;
    pixels->bytes                  = HeapVector<uint8_t>(16 * 16 * 4, 127);
    pixels->mips.push_back(pixels->bytes);
    for (uint32_t index = 0; index < 4; ++index)
    {
        UniquePtr<sg::Texture> texture = MakeUnique<sg::Texture>("spatial_page");
        texture->width = texture->height = 16;
        texture->index                   = index;
        texture->format                  = index == 1 ? asset::Format::R8G8B8A8_SRGB : pixels->format;
        texture->pixels                  = pixels;
        texture->atlasRegion             = index == 0 ? asset::ImageRegion{4, 4, 4, 8} : asset::ImageRegion{8, 4, 4, 8};
        texture->samplerIndex            = 0;
        scene.AddComponent(std::move(texture));
    }
}

TEST_F(RenderCoreTest, SpatialPagesShareOneUploadAndCompatibleViewsWithIndependentLoads)
{
    rhi->info.subTexelPrecisionBits = 8;
    sg::Scene scene;
    AddSpatialSceneTextures(scene);
    EXPECT_TRUE(RenderScene::GetResourceError(rhi->info, scene, 3, 3).empty());
    StagingBufferManager staging(4096, 4096);
    StagingUploadQueue   uploads(device, &staging);
    TextureManager       textures(device, &uploads);
    StagingAllocation    probe;
    ASSERT_EQ(staging.Allocate(16, 16, &probe), StagingFlushAction::eNone);
    TestBuffer*                 payload = static_cast<TestBuffer*>(probe.pBuffer);
    HeapVector<RHITextureView*> first, second;
    const uint32_t              before = rhi->textureCreations;
    textures.LoadSceneTextures(&scene, first);
    textures.LoadSceneTextures(&scene, second);
    ASSERT_EQ(first.size(), 4u);
    ASSERT_EQ(second.size(), 4u);
    for (const HeapVector<RHITextureView*>* views : {&first, &second})
    {
        for (uint32_t index = 0; index < 4; ++index)
        {
            ASSERT_NE((*views)[index], nullptr);
            EXPECT_EQ((*views)[index]->GetTexture(), (*views)[0]->GetTexture());
            EXPECT_EQ((*views)[index]->GetSubResourceRange().layerCount, 1u);
            EXPECT_EQ((*views)[index]->GetSubResourceRange().levelCount, 1u);
            EXPECT_EQ((*views)[index]->GetFormat(), index == 1 ? DataFormat::eR8G8B8A8SRGB : DataFormat::eR8G8B8A8UNORM);
        }
        EXPECT_EQ((*views)[0], (*views)[2]);
        EXPECT_EQ((*views)[2], (*views)[3]);
        EXPECT_NE((*views)[0], (*views)[1]);
    }
    EXPECT_NE(first[0]->GetTexture(), second[0]->GetTexture());
    EXPECT_EQ(rhi->textureCreations, before + 2);
    ASSERT_TRUE(uploads.Flush());
    uint32_t copies = 0;
    for (TestContext* context : {&rhi->graphics, &rhi->transfer})
    {
        EXPECT_TRUE(context->blits.empty());
        for (const RHIBufferTextureCopyRegion& copy : context->textureCopies)
        {
            ++copies;
            EXPECT_EQ(copy.textureSize, Vec3i(16, 16, 1));
            EXPECT_EQ(copy.textureSubresources.layerCount, 1u);
            EXPECT_EQ(copy.textureSubresources.mipmap, 0u);
            ASSERT_LE(copy.bufferOffset + 1024, payload->bytes.size());
            EXPECT_EQ(std::memcmp(payload->bytes.data() + copy.bufferOffset,
                                  scene.GetComponents<sg::Texture>()[0]->GetBytes().data(), 1024),
                      0);
        }
        context->textureCopies.clear();
    }
    EXPECT_EQ(copies, 2u);
    staging.Release(probe, {});
    textures.Destroy();
    uploads.Destroy();
    staging.Destroy();
}

TEST_F(RenderCoreTest, SpatialInvalidPayloadsAndSamplersFailBeforeAllocation)
{
    rhi->info.subTexelPrecisionBits = 8;
    for (uint32_t scenario = 0; scenario < 10; ++scenario)
    {
        SCOPED_TRACE(scenario);
        sg::Scene scene;
        AddSpatialSceneTextures(scene);
        sg::Texture& source = *scene.GetComponents<sg::Texture>()[0];
        if (scenario == 0)
        {
            source.atlasRegion.x = UINT32_MAX;
        }
        if (scenario == 1)
        {
            source.atlasRegion.width = 0;
        }
        if (scenario == 2)
        {
            source.atlasRegion.width = 3;
        }
        if (scenario == 3)
        {
            source.atlasRegion.x = 1;
        }
        if (scenario == 4)
        {
            source.width = 32;
        }
        if (scenario == 5)
        {
            source.arrayLayer = 1;
        }
        if (scenario == 6)
        {
            source.samplerIndex = -1;
        }
        if (scenario == 7)
        {
            source.samplerIndex = 100;
        }
        if (scenario == 8)
        {
            scene.GetComponents<sg::Sampler>()[0]->wrapS = asset::SamplerAddressMode::Repeat;
        }
        if (scenario == 9)
        {
            scene.GetComponents<sg::Sampler>()[0]->minFilter = asset::TextureFilter::Nearest;
        }
        // All aliases inherit this invalid view, keeping allocation counts decisive.
        for (sg::Texture* texture : scene.GetComponents<sg::Texture>())
        {
            texture->atlasRegion  = source.atlasRegion;
            texture->width        = source.width;
            texture->arrayLayer   = source.arrayLayer;
            texture->samplerIndex = source.samplerIndex;
        }
        StagingBufferManager        staging(4096, 4096);
        StagingUploadQueue          uploads(device, &staging);
        TextureManager              textures(device, &uploads);
        HeapVector<RHITextureView*> views;
        const uint32_t              before = rhi->textureCreations;
        textures.LoadSceneTextures(&scene, views);
        ASSERT_EQ(views.size(), 4u);
        for (RHITextureView* view : views)
        {
            EXPECT_EQ(view, nullptr);
        }
        EXPECT_EQ(rhi->textureCreations, before);
        EXPECT_FALSE(uploads.HasPending());
        textures.Destroy();
        uploads.Destroy();
        staging.Destroy();
    }
}

TEST_F(RenderCoreTest, SpatialViewFailureRetainsTheSharedPageForLaterAliases)
{
    rhi->info.subTexelPrecisionBits = 8;
    sg::Scene scene;
    AddSpatialSceneTextures(scene);
    StagingBufferManager        staging(4096, 4096);
    StagingUploadQueue          uploads(device, &staging);
    TextureManager              textures(device, &uploads);
    HeapVector<RHITextureView*> views;
    const uint32_t              before = rhi->textureCreations;
    rhi->failTextureViewCreationAt     = rhi->textureViewCreations + 1;
    textures.LoadSceneTextures(&scene, views);
    ASSERT_EQ(views.size(), 4u);
    ASSERT_NE(views[0], nullptr);
    EXPECT_EQ(views[1], nullptr);
    EXPECT_EQ(views[0], views[2]);
    EXPECT_EQ(views[0], views[3]);
    EXPECT_EQ(rhi->textureCreations, before + 1);
    EXPECT_TRUE(uploads.Flush());
    textures.Destroy();
    uploads.Destroy();
    staging.Destroy();
    rhi->failTextureViewCreationAt = 0;
}

TEST_F(RenderCoreTest, ArrayPagesShareUploadsAndLayerViewsWithinEachIndependentLoad)
{
    for (DataFormat format :
         {DataFormat::eR8G8B8A8UNORM, DataFormat::eBC7UNORMBlock, DataFormat::eBC4UNORMBlock, DataFormat::eBC5UNORMBlock})
    {
        sg::Scene scene;
        AddArraySceneTextures(scene, format);
        StagingBufferManager staging(4096, 4096);
        StagingUploadQueue   uploads(device, &staging);
        StagingAllocation    probe;
        ASSERT_EQ(staging.Allocate(16, 16, &probe), StagingFlushAction::eNone);
        TestBuffer*                 payload = static_cast<TestBuffer*>(probe.pBuffer);
        TextureManager              textures(device, &uploads);
        HeapVector<RHITextureView*> first, second;
        const uint32_t              before = rhi->textureCreations;
        textures.LoadSceneTextures(&scene, first);
        textures.LoadSceneTextures(&scene, second);
        ASSERT_EQ(first.size(), 3u);
        ASSERT_EQ(second.size(), 3u);
        for (const HeapVector<RHITextureView*>* batch : {&first, &second})
        {
            for (uint32_t index = 0; index < 3; ++index)
            {
                ASSERT_NE((*batch)[index], nullptr);
                EXPECT_EQ((*batch)[index]->GetTexture(), (*batch)[0]->GetTexture());
                EXPECT_EQ((*batch)[index]->GetTextureType(), RHITextureType::e2D);
                EXPECT_EQ((*batch)[index]->GetSubResourceRange().baseArrayLayer, index == 0 ? 0u : 1u);
                EXPECT_EQ((*batch)[index]->GetSubResourceRange().layerCount, 1u);
                EXPECT_EQ((*batch)[index]->GetSubResourceRange().levelCount, 3u);
                EXPECT_EQ((*batch)[index]->GetFormat(),
                          static_cast<DataFormat>(scene.GetComponents<sg::Texture>()[index]->format));
            }
            EXPECT_EQ((*batch)[1], (*batch)[2]);
        }
        EXPECT_NE(first[0]->GetTexture(), second[0]->GetTexture());
        EXPECT_EQ(rhi->textureCreations, before + 2);
        ASSERT_TRUE(uploads.Flush());
        uint32_t                  copies = 0;
        const asset::ImagePixels& source = *scene.GetComponents<sg::Texture>()[0]->pixels;
        for (TestContext* context : {&rhi->graphics, &rhi->transfer})
        {
            EXPECT_TRUE(context->blits.empty());
            for (const RHIBufferTextureCopyRegion& copy : context->textureCopies)
            {
                ++copies;
                EXPECT_EQ(copy.textureSubresources.layerCount, 2u);
                EXPECT_EQ(copy.textureSubresources.baseArrayLayer, 0u);
                const HeapVector<uint8_t>& mip = source.mips[copy.textureSubresources.mipmap];
                ASSERT_LE(copy.bufferOffset + mip.size(), payload->bytes.size());
                EXPECT_EQ(std::memcmp(payload->bytes.data() + copy.bufferOffset, mip.data(), mip.size()), 0);
            }
            context->textureCopies.clear();
        }
        EXPECT_EQ(copies, 6u);
        staging.Release(probe, {});
        textures.Destroy();
        uploads.Destroy();
        staging.Destroy();
    }
}

TEST_F(RenderCoreTest, ArrayPageViewFailureNeverPublishesOrCachesAFailedView)
{
    for (uint32_t failure : {1u, 2u})
    {
        sg::Scene scene;
        AddArraySceneTextures(scene, DataFormat::eR8G8B8A8UNORM);
        StagingBufferManager        staging(4096, 4096);
        StagingUploadQueue          uploads(device, &staging);
        TextureManager              textures(device, &uploads);
        HeapVector<RHITextureView*> views;
        const uint32_t              before = rhi->textureCreations;
        rhi->failTextureViewCreationAt     = rhi->textureViewCreations + failure;
        textures.LoadSceneTextures(&scene, views);
        ASSERT_EQ(views.size(), 3u);
        EXPECT_EQ(views[failure - 1], nullptr);
        ASSERT_NE(views[2], nullptr);
        ASSERT_NE(views[failure == 1 ? 1 : 0], nullptr);
        EXPECT_EQ(views[2]->GetTexture(), views[failure == 1 ? 1 : 0]->GetTexture());
        EXPECT_EQ(rhi->textureCreations, before + (failure == 1 ? 2u : 1u));
        EXPECT_TRUE(uploads.Flush());
        textures.Destroy();
        uploads.Destroy();
        staging.Destroy();
        rhi->failTextureViewCreationAt = 0;
    }
}

TEST_F(RenderCoreTest, ArrayPageMutableOverridesAndInvalidViewsDoNotShareThePage)
{
    sg::Scene scene;
    AddArraySceneTextures(scene, DataFormat::eR8G8B8A8UNORM);
    const HeapVector<sg::Texture*> sources = scene.GetComponents<sg::Texture>();
    sources[1]->arrayLayer                 = 2;
    sources[2]->bytesData.resize(5 * 7 * 4, 70);
    StagingBufferManager        staging(4096, 4096);
    StagingUploadQueue          uploads(device, &staging);
    TextureManager              textures(device, &uploads);
    HeapVector<RHITextureView*> views;
    textures.LoadSceneTextures(&scene, views);
    ASSERT_EQ(views.size(), 3u);
    ASSERT_NE(views[0], nullptr);
    EXPECT_EQ(views[1], nullptr);
    ASSERT_NE(views[2], nullptr);
    EXPECT_NE(views[0]->GetTexture(), views[2]->GetTexture());
    EXPECT_EQ(views[2]->GetSubResourceRange().baseArrayLayer, 0u);
    EXPECT_EQ(views[2]->GetTexture()->GetArrayLayers(), 1u);
    textures.Destroy();
    uploads.Destroy();
    staging.Destroy();
}

TEST_F(RenderCoreTest, FailedArrayAllocationOrStagingDoesNotEnterTheSharedPageCache)
{
    for (bool failStaging : {false, true})
    {
        sg::Scene source;
        AddArraySceneTextures(source, DataFormat::eR8G8B8A8UNORM);
        StagingBufferManager staging(4096, 4096);
        StagingUploadQueue   uploads(device, &staging);
        TextureManager       textures(device, &uploads);
        const uint32_t       before = rhi->textureCreations;
        if (failStaging)
        {
            rhi->failBufferCreationAt = rhi->bufferCreations + 1;
        }
        else
        {
            rhi->failTextureCreationAt = before + 1;
        }
        HeapVector<RHITextureView*> views;
        textures.LoadSceneTextures(&source, views);
        ASSERT_EQ(views.size(), 3u);
        EXPECT_EQ(views[0], nullptr);
        ASSERT_NE(views[1], nullptr);
        EXPECT_EQ(views[1], views[2]);
        EXPECT_EQ(rhi->textureCreations, before + 2);
        EXPECT_TRUE(uploads.Flush());
        textures.Destroy();
        uploads.Destroy();
        staging.Destroy();
        rhi->failBufferCreationAt  = 0;
        rhi->failTextureCreationAt = 0;
    }
}

TEST_F(RenderCoreTest, SceneRetiresArrayPageOnceAfterItsGraphAndGPUUsesComplete)
{
    sg::Scene source;
    AddArraySceneTextures(source, DataFormat::eR8G8B8A8UNORM);
    device->LoadSceneTextures(&source, sceneInputs.textures);
    ASSERT_EQ(sceneInputs.textures.size(), 3u);
    ASSERT_NE(sceneInputs.textures[0], nullptr);
    RHITexture*    image = sceneInputs.textures[0]->GetTexture();
    const uint64_t id    = image->GetStableId();
    SceneData      data{};
    data.pScene = &source;
    RenderScene scene(device, data);
    RenderGraph graph("array_lifetime");
    ASSERT_TRUE(graph.Begin());
    graph.AddTransferPass("touch_array").ClearTexture(image, Color(0.f));
    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    scene.Destroy();
    scene.Destroy();
    EXPECT_TRUE(scene.GetSceneTextureViews().empty());
    device->CollectCompletedResources();
    EXPECT_FALSE(destroyed.contains(id));
    rhi->completed = rhi->submitted;
    device->CollectCompletedResources();
    EXPECT_FALSE(destroyed.contains(id)); // The replayable graph still owns its dependency.
    graph.Reset();
    device->CollectCompletedResources();
    EXPECT_TRUE(destroyed.contains(id));
    sceneInputs = {};
}

TEST_F(RenderCoreTest, BC7SceneRejectsMalformedOrUnsupportedUploadsBeforePublication)
{
    for (uint32_t scenario = 0; scenario < 4; ++scenario)
    {
        sg::Scene              scene;
        UniquePtr<sg::Texture> source = MakeUnique<sg::Texture>("invalid_bc7");
        source->width = source->height = 5;
        source->format                 = asset::Format::BC7_UNORM_BLOCK;
        source->bytesData.resize(64, 0x40);
        source->mipBytes = {HeapVector<uint8_t>(source->bytesData.data(), source->bytesData.size()),
                            HeapVector<uint8_t>(16, 0x40), HeapVector<uint8_t>(16, 0x40)};
        if (scenario == 0)
        {
            source->mipBytes[2].pop_back();
        }
        else if (scenario == 1)
        {
            source->mipBytes[0][0] ^= 1;
        }
        else if (scenario == 2)
        {
            source->format = asset::Format::BC5_UNORM_BLOCK;
        }
        else
        {
            rhi->copyCapabilities[DataFormat::eBC7UNORMBlock] = {};
        }
        scene.AddComponent(std::move(source));
        StagingBufferManager        staging(4096, 4096);
        StagingUploadQueue          uploads(device, &staging);
        TextureManager              textures(device, &uploads);
        HeapVector<RHITextureView*> outputs;
        const uint32_t              before = rhi->textureCreations;
        textures.LoadSceneTextures(&scene, outputs);
        ASSERT_EQ(outputs.size(), 1u);
        EXPECT_EQ(outputs[0], nullptr);
        EXPECT_FALSE(uploads.HasPending());
        EXPECT_EQ(rhi->textureCreations, before + (scenario == 3 ? 1u : 0u));
        textures.Destroy();
        uploads.Destroy();
        staging.Destroy();
        rhi->copyCapabilities.clear();
    }
}

TEST_F(RenderCoreTest, BlockCopyValidationRejectsUnalignedAndTruncatedBlocks)
{
    for (DataFormat format : {DataFormat::eBC4UNORMBlock, DataFormat::eBC5UNORMBlock, DataFormat::eBC7UNORMBlock})
    {
        const uint32_t blockBytes = format == DataFormat::eBC4UNORMBlock ? 8u : 16u;
        for (uint32_t scenario = 0; scenario < 5; ++scenario)
        {
            RHITextureCreateInfo info{};
            info.type   = RHITextureType::e2D;
            info.format = format;
            info.width = info.height = 7;
            info.usageFlags.SetFlag(RHITextureUsageFlagBits::eTransferDst);
            RHITexture*                texture = rhi->CreateTexture(info);
            TestBuffer*                buffer  = Buffer(scenario == 1 ? 4 * blockBytes - 1 : 5 * blockBytes);
            RHIBufferTextureCopyRegion region{};
            region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
            region.textureSize = {7, 7, 1};
            if (scenario == 2)
            {
                region.textureOffset.x = 1;
                region.textureSize.x   = 6;
            }
            else if (scenario == 3)
            {
                region.textureSize.x = 3;
            }
            else if (scenario == 4)
            {
                region.bufferOffset = 4;
            }
            RenderGraph graph("bc7_copy_validation");
            ASSERT_TRUE(graph.Begin());
            graph.AddTransferPass("upload").CopyBufferToTexture(buffer, texture, region);
            EXPECT_EQ(graph.End(), scenario == 0);
            if (scenario == 0)
            {
                EXPECT_TRUE(device->ExecuteRenderGraph(graph));
            }
            else
            {
                EXPECT_EQ(graph.GetResult().code, RDGErrorCode::eRange);
            }
            device->DestroyBuffer(buffer);
            device->DestroyTexture(texture);
        }
    }
}

TEST_F(RenderCoreTest, BlockPoolBudgetIncludesMinimumBlocksForSmallMipsAndLayers)
{
    for (DataFormat format : {DataFormat::eBC4UNORMBlock, DataFormat::eBC5UNORMBlock, DataFormat::eBC7UNORMBlock})
    {
        RenderGraph graph("bc7_budget");
        ASSERT_TRUE(graph.Begin());
        RDGResourceManager* resources      = graph.GetResourceManager();
        RDGTextureDesc      desc           = LogicalTexture(3);
        desc.texFormat.format              = format;
        desc.texFormat.width               = 5;
        desc.texFormat.height              = 7;
        desc.texFormat.arrayLayers         = 2;
        const RDGTexture           texture = resources->CreateTexture(desc);
        TestBuffer*                buffer  = Buffer(128);
        RHIBufferTextureCopyRegion region{};
        region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
        region.textureSubresources.layerCount = 2;
        region.textureSize                    = {5, 7, 1};
        const RDGBuffer input                 = resources->ImportHostWrittenBuffer(buffer);
        graph.AddTransferPass("upload").NeverCull().CopyBufferToTexture(input, texture, region);
        ASSERT_TRUE(graph.End());
        ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
        EXPECT_EQ(resources->GetPoolStats().assignedBytes, format == DataFormat::eBC4UNORMBlock ? 96u : 192u);
        device->DestroyBuffer(buffer);
    }
}

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

    HeapVector<RHITextureView*> outputs;

    textures.LoadSceneTextures(&scene, outputs);

    ASSERT_EQ(outputs.size(), 2u);

    EXPECT_EQ(outputs[0]->GetFormat(), DataFormat::eR8G8B8A8SRGB);

    EXPECT_EQ(outputs[1]->GetFormat(), DataFormat::eR8G8B8A8UNORM);

    EXPECT_EQ(outputs[0]->GetTexture()->GetNumMipmaps(), 4u);

    EXPECT_EQ(outputs[1]->GetTexture()->GetNumMipmaps(), 4u);

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

    skybox->OnRenderGraphExecuted(RenderFrameResult::eSubmitted);

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
