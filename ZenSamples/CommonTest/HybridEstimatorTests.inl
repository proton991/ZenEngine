TEST_P(ConeVoxelGIIntegrationTest, HitRadianceRejectsThinWallBackfacesAndNormalizesCoverage)
{
    ShaderProgramManager::GetInstance().StoreProgram(
        ZEN_NEW() ComputeFileSP(device, "HitRadianceCheckSP", "VoxelGI/Calibration/hit_radiance_check.comp.spv"));
    RHITextureCreateInfo info;
    info.type  = RHITextureType::e3D;
    info.width = info.height = info.depth = 4;
    info.format                           = DataFormat::eR32G32B32A32SFloat;
    info.usageFlags.SetFlags(RHITextureUsageFlagBits::eSampled, RHITextureUsageFlagBits::eTransferDst);
    RHITexture* radiance = device->CreateTexture(info);
    RHITexture* normals  = device->CreateTexture(info);
    textures.push_back(radiance);
    textures.push_back(normals);
    HeapVector<Vec4> values(64, Vec4(0));
    HeapVector<Vec4> faces(64, Vec4(.5f));
    values[2 + 4 * (1 + 4)]                 = Vec4(1, 2, 3, .5f);
    faces[2 + 4 * (1 + 4)]                  = Vec4(0, .5f, .5f, 1);
    values[2 + 4 * (2 + 4)]                 = Vec4(100, 100, 100, 1);
    faces[2 + 4 * (2 + 4)]                  = Vec4(1, .5f, .5f, 1);
    values[1 + 4 * (2 + 4)]                 = Vec4(2, 3, 4, 1);
    faces[1 + 4 * (2 + 4)]                  = Vec4(0, .5f, .5f, 1);
    RHIBuffer*                 upload       = Buffer(64 * sizeof(Vec4), RHIBufferAllocateType::eCPUWrite, values.data());
    RHIBuffer*                 normalUpload = Buffer(64 * sizeof(Vec4), RHIBufferAllocateType::eCPUWrite, faces.data());
    RHIBuffer*                 output       = Buffer(5 * sizeof(Vec4), RHIBufferAllocateType::eGPU);
    RHIBuffer*                 readback     = Buffer(5 * sizeof(Vec4), RHIBufferAllocateType::eCPURead);
    RHIBufferTextureCopyRegion region;
    region.textureSize = Vec3i(4);
    region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    RenderGraph graph("hit_radiance_contract");
    ASSERT_TRUE(graph.Begin());
    graph.AddTransferPass("UploadRadiance").CopyBufferToTexture(upload, radiance, region);
    graph.AddTransferPass("UploadNormals").CopyBufferToTexture(normalUpload, normals, region);
    VoxelGIUniformData gi{};
    gi.gridMinVoxelSize = Vec4(0, 0, 0, 1);
    gi.volume           = Vec4(4, .25f, 1, .5f);
    RDGComputePassDesc pass;
    pass.SetShaderProgramName("HitRadianceCheckSP");
    pass.BindValue("uGISettings", gi);
    SceneUniformData scene{};
    pass.BindValue("uSceneData", scene);
    RHISampler* sampler = device->CreateSampler({});
    pass.BindSampledTexture("radiance", sampler, radiance->GetDefaultView());
    pass.BindSampledTexture("normals", sampler, normals->GetDefaultView());
    pass.BindStorageBuffer("Results", output, RDGContentGuarantee::eFullWrite);
    graph.AddComputePass(std::move(pass)).RecordPassCommands([](RDGPassCmdEncoder& encoder) { encoder.Dispatch(5, 1, 1); });
    graph.AddTransferPass("ReadHitRadiance").CopyBuffer(output, readback, {0, 0, 5 * sizeof(Vec4)}).NeverCull();
    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
    device->FlushRHIThread();
    device->WaitForIdle();
    const HeapVector<Vec4> result = ReadStaticBuffer<Vec4>(readback);
    ASSERT_EQ(result.size(), 5u);
    EXPECT_EQ(result[0], Vec4(1, 2, 3, 0));
    EXPECT_EQ(result[1], Vec4(0, 0, 0, 1));
    EXPECT_EQ(result[2], Vec4(1, 1.5f, 2, 0));
    EXPECT_EQ(result[3], Vec4(0, 0, 0, 1));
    EXPECT_EQ(result[4], Vec4(0, 0, 0, 1));
}

// Linear index of a cell in the 4^3 light-cache fixture.
static uint32_t LightCacheFixtureCell(uint32_t x, uint32_t y, uint32_t z)
{
    return x + 4 * (y + 4 * z);
}

TEST_P(ConeVoxelGIIntegrationTest, HitLightCacheEvaluatesVisibleLightsAtTheHit)
{
    ShaderProgramManager::GetInstance().StoreProgram(
        ZEN_NEW() ComputeFileSP(device, "HitLightCacheCheckSP", "VoxelGI/Calibration/hit_light_cache_check.comp.spv"));
    RHITextureCreateInfo info;
    info.type  = RHITextureType::e3D;
    info.width = info.height = info.depth = 4;
    info.format                           = DataFormat::eR32G32B32A32SFloat;
    info.usageFlags.SetFlags(RHITextureUsageFlagBits::eSampled, RHITextureUsageFlagBits::eTransferDst);
    RHITexture* radiance    = device->CreateTexture(info);
    RHITexture* normals     = device->CreateTexture(info);
    RHITexture* reflectance = device->CreateTexture(info);
    textures.push_back(radiance);
    textures.push_back(normals);
    textures.push_back(reflectance);
    HeapVector<Vec4> values(64, Vec4(0));
    HeapVector<Vec4> faces(64, Vec4(.5f, .5f, .5f, 0));
    HeapVector<Vec4> albedo(64, Vec4(0));
    // Cell (1,1,1) also holds isotropic radiance; the owner policy weights albedo by the normal's alpha.
    values[LightCacheFixtureCell(1, 1, 1)] = Vec4(.1f, .2f, .3f, 1);
    faces[LightCacheFixtureCell(1, 1, 1)]  = Vec4(1, .5f, .5f, .5f);
    albedo[LightCacheFixtureCell(1, 1, 1)] = Vec4(.5f, .25f, 1, 1);
    faces[LightCacheFixtureCell(2, 1, 1)]  = Vec4(1, .5f, .5f, 1);
    albedo[LightCacheFixtureCell(2, 1, 1)] = Vec4(1, 1, 1, 1);
    faces[LightCacheFixtureCell(2, 2, 1)]  = Vec4(1, .5f, .5f, 1);
    albedo[LightCacheFixtureCell(2, 2, 1)] = Vec4(.25f, .25f, .25f, 1);
    // A point light near cell (2,1,1), 1.1 cells from its center and 2.1 from cell (1,1,1)'s, and a directional
    // light toward +x. Injection sets the bits of near lights; lights without a bit are not visited.
    struct CacheData
    {
        glm::uvec4 lightCount;
        GPULight   lights[MaxSceneLights];
        uint32_t   masks[ZEN_VOXEL_SURFACE_CLASSES * 64];
    };
    CacheData current{};
    current.lightCount                             = glm::uvec4(2, 0, 4, 0);
    current.lights[0].positionRange                = Vec4(3.6f, 1.5f, 1.5f, 0);
    current.lights[0].directionType                = Vec4(0, 0, -1, 1);
    current.lights[0].colorIntensity               = Vec4(1, 1, 1, 4);
    current.lights[1].directionType                = Vec4(-1, 0, 0, 0);
    current.lights[1].colorIntensity               = Vec4(1, 1, 1, 2);
    CacheData previous                             = current;
    current.masks[LightCacheFixtureCell(1, 1, 1)]  = 0x80000003u;
    current.masks[LightCacheFixtureCell(2, 1, 1)]  = 0x80000001u;
    current.masks[LightCacheFixtureCell(2, 2, 1)]  = 0x80000002u;
    previous.masks[LightCacheFixtureCell(1, 1, 1)] = 0x80000000u;
    RHIBuffer*                 upload              = Buffer(64 * sizeof(Vec4), RHIBufferAllocateType::eCPUWrite, values.data());
    RHIBuffer*                 normalUpload        = Buffer(64 * sizeof(Vec4), RHIBufferAllocateType::eCPUWrite, faces.data());
    RHIBuffer*                 reflectanceUpload   = Buffer(64 * sizeof(Vec4), RHIBufferAllocateType::eCPUWrite, albedo.data());
    RHIBuffer*                 cache               = Buffer(sizeof(CacheData), RHIBufferAllocateType::eCPUWrite, &current);
    RHIBuffer*                 previousCache       = Buffer(sizeof(CacheData), RHIBufferAllocateType::eCPUWrite, &previous);
    RHIBuffer*                 output              = Buffer(6 * sizeof(Vec4), RHIBufferAllocateType::eGPU);
    RHIBuffer*                 readback            = Buffer(6 * sizeof(Vec4), RHIBufferAllocateType::eCPURead);
    RHIBufferTextureCopyRegion region;
    region.textureSize = Vec3i(4);
    region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    RenderGraph graph("hit_light_cache_contract");
    ASSERT_TRUE(graph.Begin());
    graph.AddTransferPass("UploadRadiance").CopyBufferToTexture(upload, radiance, region);
    graph.AddTransferPass("UploadNormals").CopyBufferToTexture(normalUpload, normals, region);
    graph.AddTransferPass("UploadReflectance").CopyBufferToTexture(reflectanceUpload, reflectance, region);
    VoxelGIUniformData gi{};
    gi.gridMinVoxelSize = Vec4(0, 0, 0, 1);
    gi.volume           = Vec4(4, .25f, 1, 1);
    gi.limits           = Vec4(0, 0, 0, 1);
    RDGComputePassDesc pass;
    pass.SetShaderProgramName("HitLightCacheCheckSP");
    pass.BindValue("uGISettings", gi);
    SceneUniformData scene{};
    pass.BindValue("uSceneData", scene);
    RHISampler* sampler = device->CreateSampler({});
    pass.BindSampledTexture("radiance", sampler, radiance->GetDefaultView());
    pass.BindSampledTexture("normals", sampler, normals->GetDefaultView());
    pass.BindSampledTexture("hitReflectance", sampler, reflectance->GetDefaultView());
    pass.BindStorageBuffer("HitLightCache", cache);
    pass.BindStorageBuffer("HitLightCachePrevious", previousCache);
    pass.BindStorageBuffer("Results", output, RDGContentGuarantee::eFullWrite);
    graph.AddComputePass(std::move(pass)).RecordPassCommands([](RDGPassCmdEncoder& encoder) { encoder.Dispatch(6, 1, 1); });
    graph.AddTransferPass("ReadHitLightCache").CopyBuffer(output, readback, {0, 0, 6 * sizeof(Vec4)}).NeverCull();
    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
    device->FlushRHIThread();
    device->WaitForIdle();
    const HeapVector<Vec4> result = ReadStaticBuffer<Vec4>(readback);
    ASSERT_EQ(result.size(), 6u);
    const float pi = 3.14159265359f;
    // Both lights reach the +x class of cell (1,1,1); the point light attenuates at the hit, 1.85 cells away.
    const Vec4 lit(Vec3(.1f, .2f, .3f) + (4.0f / ((3.6f - 1.75f) * (3.6f - 1.75f)) + 2.0f) * Vec3(.25f, .125f, .5f) / pi, 0);
    // The previous cache sees no light there; isotropic radiance remains.
    const Vec4 previousLit(.1f, .2f, .3f, 0);
    // Near the point light, the hit's own ray decides; the cached bit does not. Hits 2 and 3 lie 0.7 cells
    // from it, within ZEN_BRICK_RETRACE_CELLS, so brick traversal re-traces them (alpha + 2).
    const float nearLight = 4.0f / ((3.6f - 2.9f) * (3.6f - 2.9f)) / pi;
    // The +x class is absent from cell (1,2,1); the neighbor on the ray's side has it, with its reflectance.
    const Vec4 neighbor(Vec3(2.0f * .25f / pi), 0);
    const Vec4 expected[6] = {lit, previousLit, Vec4(0, 0, 0, 2), Vec4(Vec3(nearLight), 2), Vec4(0, 0, 0, 1), neighbor};
    for (uint32_t i = 0; i < 6; ++i)
    {
        SCOPED_TRACE(i);
        for (int component = 0; component < 4; ++component)
        {
            EXPECT_NEAR(result[i][component], expected[i][component], 1e-5f * (1.0f + std::abs(expected[i][component])));
        }
    }
}

// Reference accumulation can queue hundreds of near-lamp rays for one receiver; the re-trace pass
// must add every one, since the trace left them all out of its estimate.
TEST_P(ConeVoxelGIIntegrationTest, RetraceVisitsEveryQueuedRayOfAReceiver)
{
    ShaderProgramManager::GetInstance().StoreProgram(
        ZEN_NEW() ComputeFileSP(device, "RetraceChainCheckSP", "VoxelGI/Calibration/retrace_chain_check.comp.spv"));
    // Receiver 0 chains 200 rays of weight 1, newest first; receiver 1 chains 3 rays of weight 0.5.
    constexpr uint32_t   kLongChain = 200;
    constexpr uint32_t   kRays      = kLongChain + 3;
    constexpr uint32_t   kNone      = 0xffffffffu;
    HeapVector<uint32_t> rays(kRays * 8u, 0u);
    for (uint32_t ray = 0; ray < kRays; ++ray)
    {
        const bool  first   = ray == 0 || ray == kLongChain;
        const float weight  = ray < kLongChain ? 1.0f : 0.5f;
        rays[ray * 8u + 3u] = first ? kNone : ray - 1u;
        std::memcpy(&rays[ray * 8u + 7u], &weight, sizeof(weight));
    }
    const uint32_t header[5]    = {1, 1, 1, 2, kRays};
    const uint32_t receivers[4] = {0, kLongChain - 1u, 1, kRays - 1u};
    RHIBuffer*     headerBuffer = Buffer(sizeof(header), RHIBufferAllocateType::eCPUWrite, header);
    RHIBuffer*     rayBuffer =
        Buffer(static_cast<uint32_t>(rays.size() * sizeof(uint32_t)), RHIBufferAllocateType::eCPUWrite, rays.data());
    RHIBuffer*  receiverBuffer = Buffer(sizeof(receivers), RHIBufferAllocateType::eCPUWrite, receivers);
    RHIBuffer*  output         = Buffer(2 * sizeof(glm::uvec2), RHIBufferAllocateType::eGPU);
    RHIBuffer*  readback       = Buffer(2 * sizeof(glm::uvec2), RHIBufferAllocateType::eCPURead);
    RenderGraph graph("retrace_chain_contract");
    ASSERT_TRUE(graph.Begin());
    RDGComputePassDesc pass;
    pass.SetShaderProgramName("RetraceChainCheckSP");
    pass.BindStorageBuffer("HybridRetraceHeader", headerBuffer);
    pass.BindStorageBuffer("HybridRetraceRays", rayBuffer);
    pass.BindStorageBuffer("HybridRetraceReceivers", receiverBuffer);
    pass.BindStorageBuffer("Results", output, RDGContentGuarantee::eFullWrite);
    graph.AddComputePass(std::move(pass)).RecordPassCommands([](RDGPassCmdEncoder& encoder) { encoder.Dispatch(1, 1, 1); });
    graph.AddTransferPass("ReadRetraceChain").CopyBuffer(output, readback, {0, 0, 2 * sizeof(glm::uvec2)}).NeverCull();
    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
    device->FlushRHIThread();
    device->WaitForIdle();
    const HeapVector<glm::uvec2> result = ReadStaticBuffer<glm::uvec2>(readback);
    ASSERT_EQ(result.size(), 2u);
    const float    weights[2] = {static_cast<float>(kLongChain), 1.5f};
    const uint32_t counts[2]  = {kLongChain, 3};
    for (uint32_t receiver = 0; receiver < 2; ++receiver)
    {
        SCOPED_TRACE(receiver);
        float weight = 0.0f;
        std::memcpy(&weight, &result[receiver].y, sizeof(weight));
        EXPECT_EQ(result[receiver].x, counts[receiver]);
        EXPECT_FLOAT_EQ(weight, weights[receiver]);
    }
}

TEST_P(ConeVoxelGIIntegrationTest, HybridEstimatorNormalizesAndClosedGeometryBlocksSkyAndSpecular)
{
    ShaderProgramManager::GetInstance().StoreProgram(
        ZEN_NEW() ComputeFileSP(device, "HybridCheckSP", "VoxelGI/Calibration/hybrid_check.comp.spv"));
    RHITextureCreateInfo info;
    info.type  = RHITextureType::e3D;
    info.width = info.height = info.depth = 4;
    info.format                           = DataFormat::eR8G8B8A8UNORM;
    info.usageFlags.SetFlags(RHITextureUsageFlagBits::eSampled, RHITextureUsageFlagBits::eTransferDst);
    RHITexture* occupancy = device->CreateTexture(info);
    textures.push_back(occupancy);
    RHISampler* sampler  = device->CreateSampler({});
    RHIBuffer*  output   = Buffer(4 * sizeof(Vec4), RHIBufferAllocateType::eGPU);
    RHIBuffer*  readback = Buffer(4 * sizeof(Vec4), RHIBufferAllocateType::eCPURead);
    for (uint32_t scenario = 0; scenario < 3; ++scenario)
    {
        HeapVector<uint32_t> voxels(64, 0);
        for (uint32_t z = 0; z < 4; ++z)
        {
            for (uint32_t y = 0; y < 4; ++y)
            {
                for (uint32_t x = 0; x < 4; ++x)
                {
                    const bool shell = x == 0 || y == 0 || z == 0 || x == 3 || y == 3 || z == 3;
                    if ((scenario == 1 && shell) || (scenario == 2 && x == 2))
                    {
                        voxels[x + 4 * (y + 4 * z)] = 0xff000000u;
                    }
                }
            }
        }
        RHIBuffer*                 upload = Buffer(64 * sizeof(uint32_t), RHIBufferAllocateType::eCPUWrite, voxels.data());
        RHIBufferTextureCopyRegion region;
        region.textureSize = Vec3i(4);
        region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
        RenderGraph graph("hybrid_analytic_reference");
        ASSERT_TRUE(graph.Begin());
        graph.AddTransferPass("UploadHybridFixture").CopyBufferToTexture(upload, occupancy, region);
        VoxelGIUniformData gi{};
        gi.gridMinVoxelSize = Vec4(0, 0, 0, 1);
        gi.volume           = Vec4(4, .25f, 1, 0);
        RDGComputePassDesc pass;
        pass.SetShaderProgramName("HybridCheckSP");
        pass.SetPassTag("HybridAnalyticCheck");
        pass.BindValue("uGISettings", gi);
        SceneUniformData scene{};
        pass.BindValue("uSceneData", scene);
        pass.BindSampledTexture("opacity", sampler, occupancy->GetDefaultView());
        pass.BindStorageBuffer("Results", output, RDGContentGuarantee::eFullWrite);
        graph.AddComputePass(std::move(pass)).RecordPassCommands([](RDGPassCmdEncoder& encoder) { encoder.Dispatch(1, 1, 1); });
        graph.AddTransferPass("ReadHybridCheck").CopyBuffer(output, readback, {0, 0, 4 * sizeof(Vec4)}).NeverCull();
        ASSERT_TRUE(graph.End());
        ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
        device->FlushRHIThread();
        device->WaitForIdle();
        const HeapVector<Vec4> result = ReadStaticBuffer<Vec4>(readback);
        ASSERT_EQ(result.size(), 4u);
        const float capIntegral = (0.5f - std::log(4.0f / 3.0f)) / (1.0f - std::log(2.0f));
        EXPECT_NEAR(result[3].x, capIntegral, 0.005f);
        if (scenario == 0)
        {
            EXPECT_EQ(result[0].x, 1.0f);
            // Lobe directions below the surface are outside the specular integral, not occluded.
            EXPECT_EQ(result[0].y, 1.0f);
            EXPECT_EQ(result[1].w, 1.0f);
            EXPECT_EQ(result[0].z, 0.0f);
        }
        else if (scenario == 1)
        {
            EXPECT_EQ(result[0].x, 0.0f);
            EXPECT_EQ(result[0].y, 0.0f);
            EXPECT_EQ(result[1].w, 0.0f);
            EXPECT_EQ(result[0].z, 1.0f);
            EXPECT_NEAR(result[0].w, 0.0f, 1e-6f);
            EXPECT_EQ(Vec3(result[2]), Vec3(-1, 0, 0));
            EXPECT_EQ(result[2].w, 1.0f);
        }
        else
        {
            // Finite wall: this is bounded by the analytic half-space, with rays
            // escaping over its top. The CPU fixture validates the infinite limit.
            EXPECT_GT(result[0].x, .5f);
            EXPECT_LT(result[0].x, 1.0f);
            EXPECT_EQ(result[0].z, 1.0f);
            EXPECT_NEAR(result[0].w, 2.0f, 1e-6f);
            EXPECT_EQ(Vec3(result[1]), Vec3(-1, 0, 0));
        }
    }
}
