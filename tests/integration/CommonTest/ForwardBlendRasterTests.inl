#include "Math/Math.h"
#include "ForwardBlendFixture.inl"

TEST_P(ConeVoxelGIIntegrationTest, EquivalentBlendNodesPreserveSortedPixelsAndReduceNativeDraws)
{
    ForwardBlendDrawCounters counters;
    device->InitializeRendererServer();
    RendererServer& server = *device->GetRendererServer();
    server.SetRenderOption(RenderOption::ePBR);
    rc::TextureFormat format;
    format.width = format.height = 96;
    format.depth                 = 1;
    format.format                = DataFormat::eR8G8B8A8UNORM;
    RHITexture* color            = device->CreateTextureColorRT(format, {.copyUsage = true}, "ForwardBlendParityColor");
    format.format                = DataFormat::eD32SFloat;
    RHITexture* depth            = device->CreateTextureDepthStencilRT(format, {}, "ForwardBlendParityDepth");
    textures.push_back(color);
    textures.push_back(depth);
    ASSERT_NE(color, nullptr);
    ASSERT_NE(depth, nullptr);
    const RenderView view{color, depth, 96, 96};
    RHIBuffer*       packed   = Buffer(96 * 96 * sizeof(uint32_t), RHIBufferAllocateType::eGPU);
    RHIBuffer*       readback = Buffer(96 * 96 * sizeof(uint32_t), RHIBufferAllocateType::eCPURead);
    ASSERT_NE(packed, nullptr);
    ASSERT_NE(readback, nullptr);
    HeapVector<uint32_t> reference;
    uint64_t             referenceDraws = 0;
    HeapVector<uint32_t> cpuReference[2];
    for (uint32_t mode = 0; mode < 16; ++mode)
    {
        const uint32_t scenario = mode % 4;
        const bool     gpu      = mode >= 8 && SupportsForwardGPUDraws(device->GetGPUInfo(), 4);
        const bool     lit      = mode % 8 >= 4;
        server.RequestDeferredLightingRenderer()->SetGPUCullingEnabled(gpu);
        SCOPED_TRACE(scenario);
        SCOPED_TRACE(lit ? "lit" : "unlit");
        SCOPED_TRACE(gpu ? "GPU ordering" : "CPU ordering");
        ForwardBlendSceneFixture fixture(*device);
        ASSERT_TRUE(fixture.Init(scenario == 3 ? 1 : scenario));
        sg::MaterialData material     = fixture.scene->GetMaterialsData()[0];
        material.materialProperties.y = lit ? 0.0f : 1.0f;
        // Force the general permutation without enabling a lobe. Its pixels and
        // sorted draw coalescing must match the eligible basic permutation.
        material.materialProperties.w = scenario == 3 ? 1.0f : 0.0f;
        ASSERT_TRUE(fixture.scene->UpdateMaterial(0, material));
        ASSERT_TRUE(fixture.scene->CommitGeometryUpdates());
        if (lit)
        {
            SceneLight light;
            light.type         = SceneLightType::eDirectional;
            light.direction    = Vec3(0, 0, -1);
            light.intensity    = 1.0f;
            light.castsShadows = false;
            ASSERT_NE(fixture.scene->GetLights().Add(light), 0u);
        }
        ForwardDrawCache cache;
        ASSERT_TRUE(cache.Update(*fixture.scene, fixture.camera.GetViewMatrix()));
        ASSERT_EQ(cache.batches.size(), 1u);
        EXPECT_EQ(cache.batches[0].basicPBR, scenario != 3);
        // Retire environment preprocessing before measuring the repeated scene frame.
        ASSERT_EQ(server.DispatchRenderWorkloads(view), RenderFrameResult::eSubmitted);
        device->WaitForPreviousFrames();
        device->NextFrame();
        const uint64_t before = device->GetRHIThreadMetrics().native.draws;
        ASSERT_EQ(server.DispatchRenderWorkloads(view), RenderFrameResult::eSubmitted);
        device->WaitForPreviousFrames();
        const uint64_t draws = device->GetRHIThreadMetrics().native.draws - before;
        ReadForwardBlendPixels(*device, color, packed, readback);
        ASSERT_FALSE(HasFatalFailure());
        const HeapVector<uint32_t> pixels = ReadStaticBuffer<uint32_t>(readback);
        ASSERT_EQ(pixels.size(), 96u * 96u);
        if (scenario == 0)
        {
            if (gpu)
            {
                const HeapVector<uint32_t>& expected = cpuReference[lit ? 1 : 0];
                EXPECT_TRUE(std::equal(pixels.begin(), pixels.end(), expected.begin(), expected.end()));
            }
            else
            {
                cpuReference[lit ? 1 : 0] = pixels;
            }
            reference      = pixels;
            referenceDraws = draws;
            EXPECT_GT(reference[48 * 96 + 48] & 0x00ffffffu, 0u);
        }
        else if (scenario == 1 || scenario == 3)
        {
            EXPECT_TRUE(std::equal(pixels.begin(), pixels.end(), reference.begin(), reference.end()));
            EXPECT_EQ(draws + (gpu ? 0u : 3u), referenceDraws);
        }
        else
        {
            EXPECT_FALSE(std::equal(pixels.begin(), pixels.end(), reference.begin(), reference.end()));
            EXPECT_EQ(draws, referenceDraws);
        }
        device->NextFrame();
    }
}

TEST_P(ConeVoxelGIIntegrationTest, PackedMaterialUVsMatchArbitrarySetsAndSpecularGlossiness)
{
    device->InitializeRendererServer();
    RendererServer& server = *device->GetRendererServer();
    server.SetRenderOption(RenderOption::ePBR);
    rc::TextureFormat format;
    format.width = format.height = 96;
    format.depth                 = 1;
    format.format                = DataFormat::eR8G8B8A8UNORM;
    RHITexture* color            = device->CreateTextureColorRT(format, {.copyUsage = true}, "PackedUVColor");
    format.format                = DataFormat::eD32SFloat;
    RHITexture* depth            = device->CreateTextureDepthStencilRT(format, {}, "PackedUVDepth");
    textures.push_back(color);
    textures.push_back(depth);
    ASSERT_NE(color, nullptr);
    ASSERT_NE(depth, nullptr);
    const RenderView view{color, depth, 96, 96};
    RHIBuffer*       packed   = Buffer(96 * 96 * sizeof(uint32_t), RHIBufferAllocateType::eGPU);
    RHIBuffer*       readback = Buffer(96 * 96 * sizeof(uint32_t), RHIBufferAllocateType::eCPURead);
    ASSERT_NE(packed, nullptr);
    ASSERT_NE(readback, nullptr);
    for (uint32_t workflow = 0; workflow < 2; ++workflow)
    {
        HeapVector<uint32_t> reference;
        for (uint32_t mode = 0; mode < 4; ++mode)
        {
            SCOPED_TRACE(testing::Message() << "workflow=" << workflow << " mode=" << mode);
            ASSERT_TRUE(device->PrepareForSceneReplacement());
            ForwardBlendSceneFixture fixture(*device);
            UniquePtr<sg::Texture>   texture = MakeUnique<sg::Texture>("UV selection witness");
            texture->width                   = 4;
            texture->height                  = 1;
            texture->samplerIndex            = 0;
            texture->format                  = asset::Format::R8G8B8A8_UNORM;
            texture->bytesData               = {255, 0, 0, 255, 0, 255, 0, 0, 0, 0, 255, 128, 255, 255, 255, 255};
            UniquePtr<sg::Sampler> sampler   = MakeUnique<sg::Sampler>("UV selection sampler");
            sampler->magFilter = sampler->minFilter = asset::TextureFilter::Nearest;
            sampler->mipFilter                      = asset::TextureFilter::Nearest;
            sampler->wrapS = sampler->wrapT = asset::SamplerAddressMode::Repeat;
            sampler->useMipmaps             = false;
            fixture.source.AddComponent(std::move(texture));
            fixture.source.AddComponent(std::move(sampler));
            const uint32_t setCount = mode == 1 || mode == 2 ? 3u : 2u;
            ASSERT_TRUE(fixture.Init(1, false, setCount));
            ASSERT_EQ(fixture.scene->GetUVSetCount(), setCount);
            sg::MaterialData material                = fixture.scene->GetMaterialsData()[0];
            const int        uvSet                   = mode == 2 ? 2 : mode == 3 ? 0 : 1;
            material.bcTexIndex                      = 0;
            material.bcTexSet                        = uvSet;
            material.surfaceProperties.w             = static_cast<float>(workflow);
            material.featureTextures[2].properties.x = 0;
            material.featureTextures[2].properties.y = static_cast<float>(uvSet);
            ASSERT_TRUE(fixture.scene->UpdateMaterial(0, material));
            ASSERT_TRUE(fixture.scene->CommitGeometryUpdates());
            ASSERT_EQ(server.DispatchRenderWorkloads(view), RenderFrameResult::eSubmitted);
            device->WaitForPreviousFrames();
            ReadForwardBlendPixels(*device, color, packed, readback);
            ASSERT_FALSE(HasFatalFailure());
            const HeapVector<uint32_t> pixels = ReadStaticBuffer<uint32_t>(readback);
            ASSERT_EQ(pixels.size(), 96u * 96u);
            if (mode == 0)
            {
                reference = pixels;
            }
            else if (mode == 3)
            {
                EXPECT_FALSE(std::equal(pixels.begin(), pixels.end(), reference.begin(), reference.end()));
            }
            else
            {
                EXPECT_TRUE(std::equal(pixels.begin(), pixels.end(), reference.begin(), reference.end()));
            }
            device->NextFrame();
        }
    }
}

TEST_P(ConeVoxelGIIntegrationTest, OpaqueCorePreservesBlendPixelsAndTracksEligibility)
{
    device->InitializeRendererServer();
    RendererServer& server = *device->GetRendererServer();
    server.SetRenderOption(RenderOption::ePBR);
    DeferredLightingRenderer& renderer = *server.RequestDeferredLightingRenderer();
    rc::TextureFormat         format;
    format.width = format.height = 96;
    format.depth                 = 1;
    format.format                = DataFormat::eR8G8B8A8UNORM;
    RHITexture* color            = device->CreateTextureColorRT(format, {.copyUsage = true}, "OpaqueCoreColor");
    format.format                = DataFormat::eD32SFloat;
    RHITexture* depth            = device->CreateTextureDepthStencilRT(format, {}, "OpaqueCoreDepth");
    textures.push_back(color);
    textures.push_back(depth);
    ASSERT_NE(color, nullptr);
    ASSERT_NE(depth, nullptr);
    const RenderView view{color, depth, 96, 96};
    RHIBuffer*       packed   = Buffer(96 * 96 * sizeof(uint32_t), RHIBufferAllocateType::eGPU);
    RHIBuffer*       readback = Buffer(96 * 96 * sizeof(uint32_t), RHIBufferAllocateType::eCPURead);
    ASSERT_NE(packed, nullptr);
    ASSERT_NE(readback, nullptr);
    ForwardBlendSceneFixture fixture(*device);
    UniquePtr<sg::Texture>   texture = MakeUnique<sg::Texture>("core and fringe coverage");
    texture->width                   = 4;
    texture->height                  = 1;
    texture->format                  = asset::Format::R8G8B8A8_UNORM;
    texture->bytesData               = {255, 255, 255, 255, 255, 255, 255, 0, 255, 255, 255, 128, 255, 255, 255, 255};
    texture->samplerIndex            = 0;
    UniquePtr<sg::Sampler> sampler   = MakeUnique<sg::Sampler>("core sampler");
    sampler->magFilter = sampler->minFilter = sampler->mipFilter = asset::TextureFilter::Nearest;
    sampler->wrapS = sampler->wrapT = asset::SamplerAddressMode::Repeat;
    sampler->useMipmaps             = false;
    fixture.source.AddComponent(std::move(texture));
    fixture.source.AddComponent(std::move(sampler));
    ASSERT_TRUE(fixture.Init(1));
    for (asset::Vertex& vertex : fixture.vertices)
    {
        vertex.color.a = 1.0f;
    }
    ASSERT_TRUE(fixture.scene->UpdateVertices(0, MakeVecView(static_cast<const HeapVector<asset::Vertex>&>(fixture.vertices))));
    sg::MaterialData material            = fixture.scene->GetMaterialsData()[0];
    material.bcTexIndex                  = 0;
    material.bcTexSet                    = 1;
    material.textureTransforms[0].row0.w = 1;
    ASSERT_TRUE(fixture.scene->UpdateMaterial(0, material));
    HeapVector<uint32_t> reference;
    for (uint32_t mode = 0; mode < 4; ++mode)
    {
        SCOPED_TRACE(mode);
        const bool                 split = mode == 1 || mode == 2;
        const TransparencySettings settings{mode == 0 ? TransparencyCoreMode::eDisabled
                                            : split   ? TransparencyCoreMode::eForce
                                                      : TransparencyCoreMode::eAutomatic,
                                            1.0f};
        ASSERT_TRUE(fixture.scene->SetTransparencySettings(settings));
        ASSERT_TRUE(fixture.scene->CommitGeometryUpdates());
        EXPECT_EQ(fixture.scene->GetMaterialsData()[0].textureSampling.z, split ? 1.0f : 0.0f);
        EXPECT_EQ(fixture.scene->GetMaterialsData()[0].surfaceProperties.y, static_cast<float>(sg::AlphaMode::Blend));
        ForwardDrawCache drawCache;
        ASSERT_TRUE(drawCache.Update(*fixture.scene, Mat4(1.0f)));
        ASSERT_EQ(drawCache.items.size(), 4u);
        // Equivalent node transforms may merge decals, but a split fringe must
        // retain the identity exported by its own G-buffer core.
        EXPECT_EQ(drawCache.items[0].draw.nodeIndex == drawCache.items[2].draw.nodeIndex, !split);
        renderer.SetGPUCullingEnabled(mode != 1);
        ASSERT_EQ(server.DispatchRenderWorkloads(view), RenderFrameResult::eSubmitted);
        device->WaitForPreviousFrames();
        if (mode == 2)
        {
            EXPECT_EQ(renderer.GetGPUCullingStatus().candidateCount, 2u);
        }
        ReadForwardBlendPixels(*device, color, packed, readback);
        ASSERT_FALSE(HasFatalFailure());
        const HeapVector<uint32_t> pixels = ReadStaticBuffer<uint32_t>(readback);
        ASSERT_EQ(pixels.size(), 96u * 96u);
        if (mode == 0)
        {
            reference = pixels;
            EXPECT_GT(reference[48 * 96 + 48] & 0x00ffffffu, 0u);
        }
        else
        {
            uint32_t maximum = 0;
            for (size_t index = 0; index < pixels.size(); ++index)
            {
                for (uint32_t shift : {0u, 8u, 16u})
                {
                    const int difference = int((pixels[index] >> shift) & 255u) - int((reference[index] >> shift) & 255u);
                    maximum              = std::max(maximum, static_cast<uint32_t>(std::abs(difference)));
                }
            }
            EXPECT_LE(maximum, 1u);
        }
        device->NextFrame();
    }
    material.bcTexIndex = -1;
    ASSERT_TRUE(fixture.scene->UpdateMaterial(0, material));
    ASSERT_TRUE(fixture.scene->CommitGeometryUpdates());
    EXPECT_EQ(fixture.scene->GetMaterialsData()[0].textureSampling.z, 1.0f);
    fixture.vertices[0].color.a = .5f;
    ASSERT_TRUE(fixture.scene->UpdateVertices(0, MakeVecView(static_cast<const HeapVector<asset::Vertex>&>(fixture.vertices))));
    ASSERT_TRUE(fixture.scene->CommitGeometryUpdates());
    EXPECT_EQ(fixture.scene->GetMaterialsData()[0].textureSampling.z, 0.0f);
    ASSERT_TRUE(fixture.scene->SetTransparencySettings({TransparencyCoreMode::eForce, .99f}));
    ASSERT_TRUE(fixture.scene->CommitGeometryUpdates());
    EXPECT_EQ(fixture.scene->GetMaterialsData()[0].textureSampling.z, .99f);
    material.sheenColorTransmission.w = .5f;
    ASSERT_TRUE(fixture.scene->UpdateMaterial(0, material));
    ASSERT_TRUE(fixture.scene->CommitGeometryUpdates());
    EXPECT_EQ(fixture.scene->GetMaterialsData()[0].textureSampling.z, 0.0f);
    EXPECT_FALSE(fixture.scene->SetTransparencySettings({TransparencyCoreMode::eForce, 0.0f}));
}

TEST_P(ConeVoxelGIIntegrationTest, AutomaticOpaqueCoreUsesFilteredMipStatistics)
{
    device->InitializeRendererServer();
    for (uint32_t mode = 0; mode < 4; ++mode)
    {
        SCOPED_TRACE(mode);
        ASSERT_TRUE(device->PrepareForSceneReplacement());
        ForwardBlendSceneFixture fixture(*device);
        UniquePtr<sg::Texture>   texture = MakeUnique<sg::Texture>("binary alpha with partial mip");
        texture->width                   = 4;
        texture->height                  = 1;
        texture->format                  = asset::Format::R8G8B8A8_UNORM;
        texture->bytesData               = {255, 255, 255, 255, 255, 255, 255, 0, 255, 255, 255, 0, 255, 255, 255, 255};
        texture->samplerIndex            = 0;
        if (mode != 0)
        {
            texture->alphaMips.push_back({4, 2, 2, 1, 1});
        }
        if (mode == 2)
        {
            texture->mipBytes.push_back(HeapVector<uint8_t>(texture->bytesData.data(), texture->bytesData.size()));
            texture->mipBytes.push_back({255, 255, 255, 128, 255, 255, 255, 128});
            texture->mipBytes.push_back({255, 255, 255, 128});
            texture->alphaMips.push_back({2, 0, 0, 0, 0});
            texture->alphaMips.push_back({1, 0, 0, 0, 0});
        }
        UniquePtr<sg::Sampler> sampler = MakeUnique<sg::Sampler>("alpha eligibility sampler");
        sampler->magFilter = sampler->minFilter = mode == 3 ? asset::TextureFilter::Linear : asset::TextureFilter::Nearest;
        sampler->mipFilter                      = asset::TextureFilter::Nearest;
        sampler->wrapS = sampler->wrapT = asset::SamplerAddressMode::Repeat;
        sampler->useMipmaps             = mode == 2;
        fixture.source.AddComponent(std::move(texture));
        fixture.source.AddComponent(std::move(sampler));
        ASSERT_TRUE(fixture.Init(1));
        for (asset::Vertex& vertex : fixture.vertices)
        {
            vertex.color.a = 1.0f;
        }
        ASSERT_TRUE(
            fixture.scene->UpdateVertices(0, MakeVecView(static_cast<const HeapVector<asset::Vertex>&>(fixture.vertices))));
        sg::MaterialData material            = fixture.scene->GetMaterialsData()[0];
        material.bcTexIndex                  = 0;
        material.textureTransforms[0].row0.w = 1;
        ASSERT_TRUE(fixture.scene->UpdateMaterial(0, material));
        ASSERT_TRUE(fixture.scene->CommitGeometryUpdates());
        EXPECT_EQ(fixture.scene->GetMaterialsData()[0].textureSampling.z, mode < 2 ? .99f : 0.0f);
        material.baseColorFactor.a = .5f;
        ASSERT_TRUE(fixture.scene->UpdateMaterial(0, material));
        ASSERT_TRUE(fixture.scene->CommitGeometryUpdates());
        EXPECT_EQ(fixture.scene->GetMaterialsData()[0].textureSampling.z, 0.0f);
    }
}

TEST_P(ConeVoxelGIIntegrationTest, ForwardDrawCacheTracksCommittedSceneAndViewChanges)
{
    device->InitializeRendererServer();
    ForwardDrawCache cache;
    ForwardDrawKey   previous;
    const Mat4       identity(1);
    for (uint32_t replacement = 0; replacement < 2; ++replacement)
    {
        SCOPED_TRACE(replacement);
        ForwardBlendSceneFixture fixture(*device);
        ASSERT_TRUE(fixture.Init(1, true));
        RenderScene& scene = *fixture.scene;
        if (replacement != 0)
        {
            // Fresh scenes have matching local revisions; identity must reject the old snapshot.
            EXPECT_NE(scene.GetIdentity(), previous.scene);
            EXPECT_EQ(scene.GetGeometryRevision(), previous.geometryRevision);
            EXPECT_EQ(scene.GetSurfaceRevision(), previous.surfaceRevision);
            EXPECT_EQ(scene.GetRenderableNodes().size(), previous.renderables);
        }
        ASSERT_TRUE(cache.Update(scene, identity));
        ASSERT_EQ(cache.draws.size(), 1u);
        EXPECT_EQ(cache.draws[0].indexCount, 12u);
        EXPECT_FALSE(cache.Update(scene, identity));
        previous = cache.key;
        if (replacement != 0)
        {
            const Mat4 translated = glm::translate(identity, Vec3(0, 0, .25f));
            EXPECT_FALSE(cache.Update(scene, translated));
            EXPECT_FALSE(cache.Update(scene, translated));
            for (uint32_t step = 1; step <= 4; ++step)
            {
                const Mat4 slightRotation = glm::rotate(translated, glm::radians(.01f * step), Vec3(0, 1, 0));
                EXPECT_FALSE(cache.Update(scene, slightRotation));
            }
            const Mat4 accumulated = glm::rotate(translated, glm::radians(.06f), Vec3(0, 1, 0));
            EXPECT_TRUE(cache.Update(scene, accumulated));
            EXPECT_FALSE(cache.Update(scene, accumulated));
            const Mat4 rotated = glm::rotate(translated, glm::radians(180.0f), Vec3(0, 1, 0));
            EXPECT_TRUE(cache.Update(scene, rotated));
            ASSERT_EQ(cache.indices.size(), 12u);
            EXPECT_EQ(cache.indices[0], 9u);
            EXPECT_FALSE(cache.Update(scene, rotated));
            ASSERT_TRUE(cache.Update(scene, identity));
            EXPECT_EQ(cache.indices[0], 0u);

            ASSERT_TRUE(scene.SetInstanceTransform(1, glm::translate(identity, Vec3(.5f, 0, 0))));
            ASSERT_TRUE(scene.CommitGeometryUpdates());
            ASSERT_TRUE(cache.Update(scene, identity));
            EXPECT_EQ(cache.draws.size(), 4u);
            EXPECT_FALSE(cache.Update(scene, identity));

            asset::Vertex vertex = scene.GetVertices()[0];
            vertex.pos.z         = 2.0f;
            ASSERT_TRUE(scene.UpdateVertices(0, MakeVecView(static_cast<const asset::Vertex*>(&vertex), 1)));
            ASSERT_TRUE(scene.CommitGeometryUpdates());
            ASSERT_TRUE(cache.Update(scene, identity));
            EXPECT_EQ(cache.indices[0], 3u);
            EXPECT_FALSE(cache.Update(scene, identity));

            ASSERT_TRUE(scene.SetInstanceEnabled(1, false));
            ASSERT_TRUE(scene.CommitGeometryUpdates());
            ASSERT_TRUE(cache.Update(scene, identity));
            ASSERT_EQ(cache.draws.size(), 1u);
            EXPECT_EQ(cache.draws[0].indexCount, 6u);
            EXPECT_FALSE(cache.Update(scene, identity));
            ASSERT_TRUE(scene.SetInstanceEnabled(1, true));
            ASSERT_TRUE(scene.CommitGeometryUpdates());
            ASSERT_TRUE(cache.Update(scene, identity));
            EXPECT_EQ(cache.indices.size(), 12u);

            sg::MaterialData material    = scene.GetMaterialsData()[0];
            material.surfaceProperties.y = static_cast<float>(sg::AlphaMode::Opaque);
            ASSERT_TRUE(scene.UpdateMaterial(0, material));
            ASSERT_TRUE(scene.CommitGeometryUpdates());
            ASSERT_TRUE(cache.Update(scene, identity));
            EXPECT_TRUE(cache.draws.empty());
            EXPECT_EQ(cache.opaque[static_cast<uint32_t>(sg::MeshTopology::Triangles)].size(), 2u);
            EXPECT_FALSE(cache.hasBlend);
            EXPECT_FALSE(cache.Update(scene, identity));

            material.sheenColorTransmission.w = .5f;
            ASSERT_TRUE(scene.UpdateMaterial(0, material));
            ASSERT_TRUE(scene.CommitGeometryUpdates());
            ASSERT_TRUE(cache.Update(scene, identity));
            ASSERT_FALSE(cache.batches.empty());
            EXPECT_FALSE(cache.batches[0].basicPBR);
            EXPECT_EQ(cache.draws.size(), 2u);
            EXPECT_TRUE(cache.opaque[static_cast<uint32_t>(sg::MeshTopology::Triangles)].empty());
            EXPECT_FALSE(cache.hasBlend);
            EXPECT_FALSE(cache.Update(scene, identity));

            const uint64_t geometryRevision = scene.GetGeometryRevision();
            material.baseColorFactor.x      = .5f;
            ASSERT_TRUE(scene.UpdateMaterial(0, material));
            ASSERT_TRUE(scene.CommitGeometryUpdates());
            EXPECT_EQ(scene.GetGeometryRevision(), geometryRevision);
            EXPECT_TRUE(cache.Update(scene, identity));
            EXPECT_FALSE(cache.Update(scene, identity));

            ASSERT_TRUE(scene.SetMaterialVariant(0));
            ASSERT_TRUE(scene.CommitGeometryUpdates());
            ASSERT_TRUE(cache.Update(scene, identity));
            EXPECT_TRUE(cache.draws.empty());
            const HeapVector<SceneMeshDraw>& opaque = cache.opaque[static_cast<uint32_t>(sg::MeshTopology::Triangles)];
            ASSERT_EQ(opaque.size(), 2u);
            EXPECT_EQ(opaque[0].materialIndex, 1u);
            EXPECT_EQ(opaque[1].materialIndex, 1u);
            EXPECT_FALSE(cache.Update(scene, identity));
        }
    }
}
