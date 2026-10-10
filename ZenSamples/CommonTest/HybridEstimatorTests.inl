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
    RHIBuffer*  output   = Buffer(3 * sizeof(Vec4), RHIBufferAllocateType::eGPU);
    RHIBuffer*  readback = Buffer(3 * sizeof(Vec4), RHIBufferAllocateType::eCPURead);
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
        graph.AddTransferPass("ReadHybridCheck").CopyBuffer(output, readback, {0, 0, 3 * sizeof(Vec4)}).NeverCull();
        ASSERT_TRUE(graph.End());
        ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
        device->FlushRHIThread();
        device->WaitForIdle();
        const HeapVector<Vec4> result = ReadStaticBuffer<Vec4>(readback);
        ASSERT_EQ(result.size(), 3u);
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
