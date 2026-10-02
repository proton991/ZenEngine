struct ConeVisibilityRay
{
    Vec4 origin;

    Vec4 directionMax;
};

uint32_t VisibilityRandom(uint32_t& state)
{
    state ^= state << 13;

    state ^= state >> 17;

    state ^= state << 5;

    return state;
}

HeapVector<ConeVisibilityRay> VisibilityRays(uint32_t side)
{
    // The first rays have explicit, independent hit/miss expectations against the wall.
    HeapVector<ConeVisibilityRay> rays = {{Vec4(0.5f, 1.5f, 1.5f, 0), Vec4(1, 0, 0, 1e20f)},
                                          {Vec4(0.5f, 1.5f, 1.5f, 0), Vec4(-1, 0, 0, 1e20f)},
                                          {Vec4(0.5f, 1.5f, 1.5f, 0), Vec4(1, 0, 0, side / 2.0f - 0.5f)},
                                          {Vec4(0.5f, 1.5f, 1.5f, 0), Vec4(1, 0, 0, side / 2.0f)},
                                          {Vec4(-0.5f, 1.5f, 1.5f, 0), Vec4(1, 0, 0, 1e20f)},
                                          {Vec4(side / 2.0f, 1.5f, 1.5f, 0), Vec4(-1, 0, 0, 1e20f)}};

    uint32_t random                    = 0x936baf21;

    for (uint32_t index = 0; index < 16384; ++index)
    {
        Vec3 origin;

        Vec3 direction;

        for (uint32_t axis = 0; axis < 3; ++axis)
        {
            origin[axis]    = static_cast<float>(VisibilityRandom(random) % (side * 16 + 32)) / 16 - 1;

            direction[axis] = static_cast<float>(VisibilityRandom(random) % 2049) / 1024 - 1;
        }

        switch (index % 8)
        {
            case 0: direction = Vec3(1, 0, 0); break;
            case 1: direction = Vec3(0, -1, 0); break;
            case 2: direction = Vec3(0, 0, 1); break;
            case 3: direction = Vec3(1, 1, 1); break;
            case 4: direction = Vec3(-1, -1, 0); break;
            case 5: direction = Vec3(1e-10f, -1, 1e-8f); break;
            default: break;
        }

        if (index % 8 == 3 || index % 8 == 4)
        {
            origin = Vec3(std::floor(origin.x));
        }

        if (glm::dot(direction, direction) == 0)
        {
            direction.z = 1;
        }

        const float distance = index % 3 == 0 ? 1e20f : static_cast<float>(VisibilityRandom(random) % (side * 16)) / 16;

        rays.push_back({Vec4(origin, 0), Vec4(glm::normalize(direction), distance)});
    }

    return rays;
}

TEST_P(ConeVoxelGIIntegrationTest, ConeVisibilityPreservesScalarHitsAndFiniteSegments)
{
    for (uint32_t side : {64u, 128u, 256u})
    {
        SCOPED_TRACE(side);

        RenderGraph& graph = *device->GetCurrentFrameRDG();

        ASSERT_TRUE(graph.Begin());

        HeapVector<uint32_t> occupancy(side * side * side, 0);

        uint32_t random = 0x32f2b698;

        for (uint32_t z = 0; z < side; ++z)
        {
            for (uint32_t y = 0; y < side; ++y)
            {
                for (uint32_t x = 0; x < side; ++x)
                {
                    // Sparse black occluders and a one-cell wall; opacity is independent of RGB.
                    const bool occupied                  = x == side / 2 || (y > 2 && VisibilityRandom(random) % 127 == 0);

                    occupancy[x + side * (y + side * z)] = occupied ? 0xff000000u : 0;
                }
            }
        }

        RHITextureCreateInfo info;

        info.type  = RHITextureType::e3D;

        info.width = info.height = info.depth = side;

        info.format                           = DataFormat::eR8G8B8A8UNORM;

        info.usageFlags.SetFlags(RHITextureUsageFlagBits::eSampled, RHITextureUsageFlagBits::eTransferDst);

        RHITexture* opacity = device->CreateTexture(info);

        textures.push_back(opacity);

        const HeapVector<ConeVisibilityRay> rays = VisibilityRays(side);

        const uint32_t count                     = static_cast<uint32_t>(rays.size());

        const uint32_t bytes                     = count * sizeof(Vec2);

        RHIBuffer* upload   = Buffer(side * side * side * sizeof(uint32_t), RHIBufferAllocateType::eCPUWrite, occupancy.data());

        RHIBuffer* input    = Buffer(count * sizeof(ConeVisibilityRay), RHIBufferAllocateType::eCPUWrite, rays.data());

        RHIBuffer* output   = Buffer(bytes, RHIBufferAllocateType::eGPU);

        RHIBuffer* readback = Buffer(bytes, RHIBufferAllocateType::eCPURead);

        graph.GetResourceManager()->ImportHostWrittenBuffer(upload);

        graph.GetResourceManager()->ImportHostWrittenBuffer(input);

        RHIBufferTextureCopyRegion region;

        region.textureSize = Vec3i(side);

        region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);

        graph.AddTransferPass("VisibilityOpacity").CopyBufferToTexture(upload, opacity, region);

        RDGComputePassDesc pass;

        pass.SetShaderProgramName("VoxelVisibilityCheckSP");

        pass.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);

        pass.BindSampledTexture("opacity", device->CreateSampler({}), opacity->GetDefaultView());

        pass.BindValue("uGISettings",
                       VoxelGIUniformData{Vec4(0, 0, 0, 1), Vec4(side, 1.0f / side, 1, 1), Vec4(1), Vec4(6, 128, 1, 0)});

        pass.BindValue("uSceneData", SceneUniformData{});

        pass.BindStorageBuffer("VisibilityRays", input);

        pass.BindStorageBuffer("VisibilityResults", output, RDGContentGuarantee::eFullWrite);

        graph.AddComputePass(std::move(pass)).RecordPassCommands([count](RDGPassCmdEncoder& encoder) {
            encoder.Dispatch((count + 63) / 64, 1, 1);
        });

        graph.AddTransferPass("ReadVisibility").CopyBuffer(output, readback, {0, 0, bytes}).NeverCull();

        ASSERT_TRUE(graph.End());

        ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;

        device->FlushRHIThread();

        device->WaitForIdle();

        const HeapVector<Vec2> results = ReadStaticBuffer<Vec2>(readback);

        ASSERT_EQ(results.size(), count);

        for (uint32_t index = 0; index < count; ++index)
        {
            ASSERT_EQ(results[index].x, results[index].y) << "ray " << index;
        }

        for (uint32_t index = 0; index < 6; ++index)
        {
            constexpr float expected[] = {0, 1, 1, 0, 1, 0};

            EXPECT_EQ(results[index].x, expected[index]) << "known ray " << index;
        }
    }
}
