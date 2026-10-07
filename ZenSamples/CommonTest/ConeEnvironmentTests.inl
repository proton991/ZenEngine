TEST_P(ConeVoxelGIIntegrationTest, EnvironmentConesFilterRadianceAndPreserveVisibility)
{
    ShaderProgramManager::GetInstance().StoreProgram(
        ZEN_NEW() ComputeFileSP(device, "ConeEnvironmentCheckSP", "VoxelGI/Calibration/environment_check.comp.spv"));

    // The sharp checker has the same mean as every filtered level. Sampling mip 0
    // with a handful of directions aliases; a diffuse cone must recover that mean.
    constexpr uint32_t                     side   = 64;
    constexpr uint32_t                     levels = 7;
    const Vec3                             mean(0.25f, 0.5f, 0.75f);
    HeapVector<Vec4>                       pixels;
    HeapVector<RHIBufferTextureCopyRegion> regions;
    for (uint32_t face = 0; face < 6; ++face)
    {
        for (uint32_t mip = 0; mip < levels; ++mip)
        {
            const uint32_t             extent = side >> mip;
            RHIBufferTextureCopyRegion region;
            region.bufferOffset = pixels.size() * sizeof(Vec4);
            region.textureSize  = Vec3i(extent, extent, 1);
            region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
            region.textureSubresources.mipmap         = mip;
            region.textureSubresources.baseArrayLayer = face;
            regions.push_back(region);
            for (uint32_t y = 0; y < extent; ++y)
            {
                for (uint32_t x = 0; x < extent; ++x)
                {
                    const float scale = mip == 0 ? ((x + y) % 2 == 0 ? 0.0f : 2.0f) : 1.0f;
                    pixels.push_back(Vec4(mean * scale, 1.0f));
                }
            }
        }
    }
    RHITextureCreateInfo cubeInfo;
    cubeInfo.type  = RHITextureType::eCube;
    cubeInfo.width = cubeInfo.height = side;
    cubeInfo.arrayLayers             = 6;
    cubeInfo.mipmaps                 = levels;
    cubeInfo.format                  = DataFormat::eR32G32B32A32SFloat;
    cubeInfo.usageFlags.SetFlags(RHITextureUsageFlagBits::eSampled, RHITextureUsageFlagBits::eTransferDst);
    RHITexture* environment = device->CreateTexture(cubeInfo);
    textures.push_back(environment);

    RHITextureCreateInfo volumeInfo;
    volumeInfo.type  = RHITextureType::e3D;
    volumeInfo.width = volumeInfo.height = volumeInfo.depth = 4;
    volumeInfo.format                                       = DataFormat::eR8G8B8A8UNORM;
    volumeInfo.usageFlags.SetFlags(RHITextureUsageFlagBits::eSampled, RHITextureUsageFlagBits::eTransferDst);
    RHITexture* radiance = device->CreateTexture(volumeInfo);
    RHITexture* opacity  = device->CreateTexture(volumeInfo);
    textures.push_back(radiance);
    textures.push_back(opacity);

    HeapVector<Vec4> normals;
    for (uint32_t index = 0; index < 64; ++index)
    {
        const float z      = 1.0f - 2.0f * (index + 0.5f) / 64.0f;
        const float radius = std::sqrt(1.0f - z * z);
        const float phi    = index * 2.39996323f;
        normals.push_back(Vec4(radius * std::cos(phi), radius * std::sin(phi), z, 0));
    }
    normals[0]           = Vec4(0, 1, 0, 0);
    const uint32_t bytes = static_cast<uint32_t>(normals.size() * sizeof(Vec4));
    RHIBuffer*     upload =
        Buffer(static_cast<uint32_t>(pixels.size() * sizeof(Vec4)), RHIBufferAllocateType::eCPUWrite, pixels.data());
    RHIBuffer*     input        = Buffer(bytes, RHIBufferAllocateType::eCPUWrite, normals.data());
    RHIBuffer*     output       = Buffer(bytes, RHIBufferAllocateType::eGPU);
    RHIBuffer*     readback     = Buffer(bytes, RHIBufferAllocateType::eCPURead);
    const uint32_t blockerPixel = 0xff000000u;
    RHIBuffer*     blocker      = Buffer(sizeof(blockerPixel), RHIBufferAllocateType::eCPUWrite, &blockerPixel);

    HeapVector<uint32_t> closedShell(64, 0);
    for (uint32_t z = 0; z < 4; ++z)
    {
        for (uint32_t y = 0; y < 4; ++y)
        {
            for (uint32_t x = 0; x < 4; ++x)
            {
                if (x == 0 || x == 3 || y == 0 || y == 3 || z == 0 || z == 3)
                {
                    closedShell[x + 4 * (y + 4 * z)] = blockerPixel;
                }
            }
        }
    }
    RHIBuffer* shell                 = Buffer(64 * sizeof(uint32_t), RHIBufferAllocateType::eCPUWrite, closedShell.data());

    RHISamplerCreateInfo samplerInfo = RHISamplerCreateInfo::CreateLinearRepeat();
    samplerInfo.maxLod               = static_cast<float>(levels - 1);
    RHISampler* sampler              = device->CreateSampler(samplerInfo);
    bool        initialized          = false;
    for (float aperture : {10.0f, 60.0f, 90.0f})
    {
        for (uint32_t cones : {4u, 6u})
        {
            for (uint32_t scenario = 0; scenario < 7; ++scenario)
            {
                SCOPED_TRACE(testing::Message() << aperture << " degrees, " << cones << " cones, scenario " << scenario);
                RenderGraph& graph = *device->GetCurrentFrameRDG();
                ASSERT_TRUE(graph.Begin());
                if (!initialized)
                {
                    graph.GetResourceManager()->ImportHostWrittenBuffer(upload);
                    graph.GetResourceManager()->ImportHostWrittenBuffer(input);
                    graph.GetResourceManager()->ImportHostWrittenBuffer(blocker);
                    graph.GetResourceManager()->ImportHostWrittenBuffer(shell);
                    RDGTransferPassCmdRecorder transfer = graph.AddTransferPass("InitializeConeEnvironment");
                    for (const RHIBufferTextureCopyRegion& region : regions)
                    {
                        transfer.CopyBufferToTexture(upload, environment, region);
                    }
                }
                // Scenario 6 gives the cone march partial black opacity without occupancy.
                // Transmittance drops, but every visibility ray escapes, so sky must not be attenuated twice.
                graph.AddTransferPass("ConeEnvironmentRadiance")
                    .ClearTexture(radiance, scenario == 6 ? Color(0.0f, 0.0f, 0.0f, 0.5f) : Color(0));
                graph.AddTransferPass("ConeEnvironmentVisibility").ClearTexture(opacity, scenario == 1 ? Color(1) : Color(0));
                if (scenario == 4)
                {
                    // A single overhead voxel hits the vertical axis, but covers
                    // only part of its cone. It must not black out the entire cone.
                    RHIBufferTextureCopyRegion region;
                    region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
                    region.textureOffset = Vec3i(2, 3, 2);
                    region.textureSize   = Vec3i(1);
                    graph.AddTransferPass("PartialEnvironmentBlocker").CopyBufferToTexture(blocker, opacity, region);
                }
                else if (scenario == 5)
                {
                    // An empty interior enclosed by one-voxel walls still has no sky visibility.
                    RHIBufferTextureCopyRegion region;
                    region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
                    region.textureSize = Vec3i(4);
                    graph.AddTransferPass("ClosedEnvironmentRoom").CopyBufferToTexture(shell, opacity, region);
                }
                SceneUniformData scene;
                scene.environment = Vec4(0.75f, 0, scenario == 3 ? 0.0f : 1.0f, 0);
                VoxelGIUniformData gi{Vec4(0, 0, 0, 1), Vec4(4, 0.25f, 1, 1),
                                      Vec4(std::tan(glm::radians(aperture) * 0.5f), 1, 0.5f, 2), Vec4(cones, 128, 0, 0),
                                      Vec4(0, scenario == 2 ? 0.0f : 1.0f, 0, 0)};
                RDGComputePassDesc pass;
                pass.SetShaderProgramName("ConeEnvironmentCheckSP");
                pass.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
                pass.BindValue("uGISettings", gi);
                pass.BindValue("uGIVisibilityBounds", VoxelGIVisibilityBounds{Vec4(0), Vec4(4)});
                pass.BindValue("uSceneData", scene);
                pass.BindSampledTexture("voxelRadiance", sampler, radiance->GetDefaultView());
                pass.BindSampledTexture("voxelOpacity", sampler, opacity->GetDefaultView());
                pass.BindSampledTexture("coneEnvironmentMap", sampler, environment->GetDefaultView());
                pass.BindStorageBuffer("EnvironmentNormals", input);
                pass.BindStorageBuffer("EnvironmentResults", output, RDGContentGuarantee::eFullWrite);
                graph.AddComputePass(std::move(pass)).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
                    encoder.Dispatch(1, 1, 1);
                });
                graph.AddTransferPass("ReadConeEnvironment").CopyBuffer(output, readback, {0, 0, bytes}).NeverCull();
                ASSERT_TRUE(graph.End());
                ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
                device->FlushRHIThread();
                device->WaitForIdle();
                initialized                    = true;
                const HeapVector<Vec4> results = ReadStaticBuffer<Vec4>(readback);
                ASSERT_EQ(results.size(), normals.size());
                const Vec3 expected = scenario == 0 || scenario == 6 ? mean * scene.environment.x : Vec3(0);
                for (const Vec4& result : results)
                {
                    for (uint32_t channel = 0; channel < 3; ++channel)
                    {
                        if (scenario == 4)
                        {
                            EXPECT_GE(result[channel], 0.0f);
                            EXPECT_LE(result[channel], mean[channel] * scene.environment.x + 0.0001f);
                        }
                        else
                        {
                            EXPECT_NEAR(result[channel], expected[channel], 0.0001f);
                        }
                    }
                }
                if (scenario == 4)
                {
                    EXPECT_GT(results[0].w, 0.0f);
                    EXPECT_LT(results[0].w, 1.0f);
                    EXPECT_GT(results[0].x, 0.0f);
                }
            }
        }
    }
}
