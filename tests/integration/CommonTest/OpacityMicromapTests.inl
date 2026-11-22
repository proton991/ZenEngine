TEST_P(RayQueryIntegrationTest, FourStateMicromapsKeepUnknownCandidateFallbackAndGraphLifetime)
{
    if (device->GetGPUInfo().rayQuery.opacityMicromap)
    {
        ShaderProgramManager::GetInstance().StoreProgram(
            ZEN_NEW() ComputeFileSP(device, "MicromapQueryCheckSP", "VoxelGI/Calibration/ray_query_micromap_check.comp.spv"));
        RHIFlags<RHIBufferUsageFlagBits> inputUsage;
        inputUsage.SetFlags(RHIBufferUsageFlagBits::eDeviceAddress, RHIBufferUsageFlagBits::eAccelerationStructureInput,
                            RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer);
        struct Triangle
        {
            uint32_t offset;
            uint16_t level;
            uint16_t format;
        };
        const Triangle                    triangles[] = {{0, 0, 2}, {1, 0, 2}, {2, 0, 2}, {3, 0, 2}};
        const uint8_t                     states[]    = {0, 1, 2, 3};
        RHIOpacityMicromapUsage           usage{4, 0};
        RHIAccelerationStructureBuildInfo micromap;
        micromap.description.type     = RHIAccelerationStructureType::eOpacityMicromap;
        RHIOpacityMicromapBuild& data = micromap.description.micromap;
        data.pDataBuffer              = Buffer(256 + sizeof(states), inputUsage);
        data.pTriangleBuffer          = Buffer(sizeof(triangles), inputUsage, triangles);
        ASSERT_NE(data.pDataBuffer, nullptr);
        ASSERT_NE(data.pTriangleBuffer, nullptr);
        data.dataOffset = (256 - data.pDataBuffer->GetDeviceAddress() % 256) % 256;
        data.dataSize   = sizeof(states);
        std::memcpy(static_cast<uint8_t*>(data.pDataBuffer->Map()) + data.dataOffset, states, sizeof(states));
        data.pDataBuffer->Unmap();
        data.usages           = MakeVecView(&usage, 1);
        micromap.pDestination = Structure(micromap.description);
        ASSERT_NE(micromap.pDestination, nullptr);

        Vec3           positions[12];
        uint32_t       indices[12];
        const uint32_t mappings[] = {0, 1, 2, 3};
        for (uint32_t triangle = 0; triangle < 4; ++triangle)
        {
            const float x               = float(triangle) * 2.0f;
            positions[triangle * 3]     = Vec3(x - .5f, -.5f, 0);
            positions[triangle * 3 + 1] = Vec3(x + .5f, -.5f, 0);
            positions[triangle * 3 + 2] = Vec3(x, .5f, 0);
            for (uint32_t corner = 0; corner < 3; ++corner)
            {
                indices[triangle * 3 + corner] = triangle * 3 + corner;
            }
        }
        RHIAccelerationStructureGeometry geometry;
        geometry.pVertexBuffer       = Buffer(sizeof(positions), inputUsage, positions);
        geometry.pIndexBuffer        = Buffer(sizeof(indices), inputUsage, indices);
        geometry.vertexCount         = 12;
        geometry.vertexStride        = sizeof(Vec3);
        geometry.indexCount          = 12;
        geometry.opaque              = false;
        geometry.pOpacityMicromap    = micromap.pDestination;
        geometry.pOpacityIndexBuffer = Buffer(sizeof(mappings), inputUsage, mappings);
        RHIAccelerationStructureBuildInfo blas;
        blas.description.geometries = MakeVecView(&geometry, 1);
        blas.pDestination           = Structure(blas.description, MakeVecView(&micromap.pDestination, 1));
        ASSERT_NE(blas.pDestination, nullptr);
        RHIAccelerationStructureInstance instance;
        instance.pStructure = blas.pDestination;
        RHIAccelerationStructureBuildInfo tlas;
        tlas.description.type      = RHIAccelerationStructureType::eTopLevel;
        tlas.description.instances = MakeVecView(&instance, 1);
        tlas.pDestination          = Structure(tlas.description, MakeVecView(&blas.pDestination, 1));
        ASSERT_NE(tlas.pDestination, nullptr);
        for (RHIAccelerationStructureBuildInfo* build : {&micromap, &blas, &tlas})
        {
            AllocateBuildScratch(*build);
            ASSERT_NE(build->pScratchBuffer, nullptr);
        }
        RHIBuffer* result = Buffer(8 * sizeof(Vec4), inputUsage, nullptr, RHIBufferAllocateType::eGPU);
        RHIBuffer* readback =
            Buffer(8 * sizeof(Vec4), RHIFlags<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferDstBuffer), nullptr,
                   RHIBufferAllocateType::eCPURead);
        ASSERT_NE(result, nullptr);
        ASSERT_NE(readback, nullptr);
        RenderGraph& graph = *device->GetCurrentFrameRDG();
        ASSERT_TRUE(graph.Begin());
        for (RHIBuffer* buffer : {data.pDataBuffer, data.pTriangleBuffer, geometry.pVertexBuffer, geometry.pIndexBuffer,
                                  geometry.pOpacityIndexBuffer})
        {
            graph.GetResourceManager()->ImportBuffer(buffer, RDGImportContents::eDefined);
        }
        graph.AddTransferPass("Micromap").BuildAccelerationStructure(micromap);
        graph.AddTransferPass("MicromapBLAS").BuildAccelerationStructure(blas);
        graph.AddTransferPass("MicromapTLAS").BuildAccelerationStructure(tlas);
        // Recorded commands must own the nested usage array and geometry descriptors.
        usage.count               = 0;
        geometry.pOpacityMicromap = nullptr;
        RDGComputePassDesc pass;
        pass.SetShaderProgramName("MicromapQueryCheckSP");
        pass.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
        pass.BindAccelerationStructure("testScene", tlas.pDestination);
        pass.BindStorageBuffer("Results", result, RDGContentGuarantee::eFullWrite);
        graph.AddComputePass(std::move(pass)).RecordPassCommands([](RDGPassCmdEncoder& encoder) { encoder.Dispatch(8, 1, 1); });
        graph.AddTransferPass("ReadMicromapQueries").CopyBuffer(result, readback, {0, 0, 8 * sizeof(Vec4)});
        RHIBuffer* pipelineReadback = nullptr;
        if (device->GetGPUInfo().rayTracing.IsUsable())
        {
            const HeapVector<ShaderProgram::StageFile> modules = {
                {RHIShaderStage::eRayGeneration, "VoxelGI/Calibration/micromap_rt_check.rgen.spv"},
                {RHIShaderStage::eMiss, "VoxelGI/Calibration/scene_rt_check.rmiss.spv"},
                {RHIShaderStage::eClosestHit, "VoxelGI/Calibration/micromap_rt_check.rchit.spv"},
                {RHIShaderStage::eAnyHit, "VoxelGI/Calibration/micromap_rt_check.rahit.spv"}};
            ShaderProgramManager::GetInstance().StoreProgram(ZEN_NEW() RayTracingFileSP(device, "MicromapRTCheckSP", modules));
            RHIBuffer* pipelineOutput = Buffer(8 * sizeof(Vec4), inputUsage, nullptr, RHIBufferAllocateType::eGPU);
            pipelineReadback =
                Buffer(8 * sizeof(Vec4), RHIFlags<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferDstBuffer), nullptr,
                       RHIBufferAllocateType::eCPURead);
            ASSERT_NE(pipelineOutput, nullptr);
            ASSERT_NE(pipelineReadback, nullptr);
            RDGRayTracingPassDesc pipelinePass;
            pipelinePass.SetShaderProgramName("MicromapRTCheckSP");
            pipelinePass.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
            pipelinePass.SetShaderGroups({{RHIRayTracingShaderGroupType::eRayGeneration, 0},
                                          {RHIRayTracingShaderGroupType::eMiss, 1},
                                          {RHIRayTracingShaderGroupType::eHit, kUnusedRayTracingShader, 2, 3}});
            pipelinePass.BindAccelerationStructure("testScene", tlas.pDestination);
            pipelinePass.BindStorageBuffer("Results", pipelineOutput, RDGContentGuarantee::eFullWrite);
            graph.AddRayTracingPass(std::move(pipelinePass)).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
                encoder.TraceRays(8, 1);
            });
            graph.AddTransferPass("ReadMicromapPipeline")
                .CopyBuffer(pipelineOutput, pipelineReadback, {0, 0, 8 * sizeof(Vec4)});
        }
        ASSERT_TRUE(graph.End()) << graph.GetResult().message;
        ASSERT_TRUE(device->ExecuteRenderGraph(graph));
        CheckQueryQueue(graph, pipelineReadback != nullptr ? 2 : 1);
        device->WaitForIdle();
        const Vec4* values = reinterpret_cast<const Vec4*>(readback->Map());
        ASSERT_NE(values, nullptr);
        for (uint32_t index = 0; index < 8; ++index)
        {
            SCOPED_TRACE(index);
            const bool hit = index == 2 || index == 3 || index == 4 || index == 6;
            EXPECT_FLOAT_EQ(values[index].x, hit ? 1.0f : -1.0f);
            EXPECT_FLOAT_EQ(values[index].y, index >= 4 ? 1.0f : 0.0f);
        }
        if (pipelineReadback != nullptr)
        {
            const Vec4* pipelineValues = reinterpret_cast<const Vec4*>(pipelineReadback->Map());
            ASSERT_NE(pipelineValues, nullptr);
            EXPECT_EQ(std::memcmp(pipelineValues, values, 8 * sizeof(Vec4)), 0);
            pipelineReadback->Unmap();
        }
        readback->Unmap();
    }
    else
    {
        GTEST_SKIP() << "Four-state opacity micromaps unavailable";
    }
}

TEST_P(RayQueryIntegrationTest, BakedMicromapSubdivisionsPreserveCoverageAndReduceCallbacks)
{
    if (device->GetGPUInfo().rayQuery.opacityMicromap)
    {
        ShaderProgramManager::GetInstance().StoreProgram(
            ZEN_NEW() ComputeFileSP(device, "MicromapBakeSP", "VoxelGI/opacity_micromap_bake.comp.spv"));
        ShaderProgramManager::GetInstance().StoreProgram(ZEN_NEW() ComputeFileSP(
            device, "MicromapBakeQuerySP", "VoxelGI/Calibration/ray_query_micromap_bake_check.comp.spv"));
        RHIFlags<RHIBufferUsageFlagBits> inputUsage;
        inputUsage.SetFlags(RHIBufferUsageFlagBits::eDeviceAddress, RHIBufferUsageFlagBits::eAccelerationStructureInput,
                            RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer,
                            RHIBufferUsageFlagBits::eTransferDstBuffer);
        const Vec4       corners[]   = {Vec4(0, 0, 0, 0), Vec4(1, 0, 1, 0), Vec4(0, 1, 0, 0)};
        const Vec3       positions[] = {Vec3(0, 0, 0), Vec3(1, 0, 0), Vec3(0, 1, 0)};
        const uint32_t   indices[]   = {0, 1, 2};
        const uint32_t   mapping     = 0;
        sg::MaterialData material{};
        material.baseColorFactor   = Vec4(1);
        material.surfaceProperties = Vec4(.5f, 2, 1, 0);
        RHIBuffer* cornerBuffer    = Buffer(sizeof(corners), inputUsage, corners);
        RHIBuffer* materialBuffer  = Buffer(sizeof(material), inputUsage, &material);
        RHIBuffer* vertexBuffer    = Buffer(sizeof(positions), inputUsage, positions);
        RHIBuffer* indexBuffer     = Buffer(sizeof(indices), inputUsage, indices);
        RHIBuffer* mappingBuffer   = Buffer(sizeof(mapping), inputUsage, &mapping);
        RHIBuffer* result          = Buffer(128 * 128 * sizeof(Vec4), inputUsage, nullptr, RHIBufferAllocateType::eGPU);
        RHIBuffer* readback =
            Buffer(128 * 128 * sizeof(Vec4), RHIFlags<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferDstBuffer),
                   nullptr, RHIBufferAllocateType::eCPURead);
        sg::Scene              textureScene;
        UniquePtr<sg::Texture> source = MakeUnique<sg::Texture>("Micromap checker coverage");
        source->width = source->height = 8;
        source->format                 = asset::Format::R8G8B8A8_UNORM;
        source->bytesData.resize(8 * 8 * 4, 255);
        for (uint32_t y = 0; y < 8; ++y)
        {
            for (uint32_t x = 0; x < 8; ++x)
            {
                source->bytesData[(y * 8 + x) * 4 + 3] = (x + y) % 3 == 0 ? 0 : 255;
            }
        }
        textureScene.AddComponent(std::move(source));
        HeapVector<RHITextureView*> views;
        device->LoadSceneTextures(&textureScene, views);
        ASSERT_EQ(views.size(), 1u);
        for (uint32_t mode = 0; mode < 4; ++mode)
        {
            SCOPED_TRACE(mode);
            ASSERT_TRUE(device->PrepareForSceneReplacement());
            RHISamplerCreateInfo samplerInfo = RHISamplerCreateInfo::CreateLinearRepeat();
            if (mode == 2)
            {
                samplerInfo.magFilter = samplerInfo.minFilter = RHISamplerFilter::eNearest;
                samplerInfo.repeatU = samplerInfo.repeatV = RHISamplerRepeatMode::eMirroredRepeat;
            }
            else if (mode == 3)
            {
                samplerInfo.repeatU = samplerInfo.repeatV = RHISamplerRepeatMode::eClampToEdge;
            }
            RHISampler* sampler = device->CreateSampler(samplerInfo);
            ASSERT_NE(sampler, nullptr);
            material.bcTexIndex                      = mode == 0 ? -1 : 0;
            material.surfaceProperties.w             = mode == 3 ? 1.0f : 0.0f;
            material.featureTextures[2].properties.x = 0;
            material.textureTransforms[0].row0       = Vec4(1.7f, .31f, -.2f, 0);
            material.textureTransforms[0].row1       = Vec4(-.17f, 1.3f, .25f, 0);
            material.featureTextures[2].transform    = material.textureTransforms[0];
            std::memcpy(materialBuffer->Map(), &material, sizeof(material));
            materialBuffer->Unmap();
            uint32_t previousCallbacks = UINT32_MAX;
            for (uint32_t level : {1u, 2u, 4u, 6u})
            {
                SCOPED_TRACE(level);
                const uint32_t                    bytes      = ((1u << (2 * level)) + 15u) / 16u * sizeof(uint32_t);
                const uint32_t                    triangle[] = {0, level | (2u << 16u)};
                RHIOpacityMicromapUsage           usage{1, level};
                RHIAccelerationStructureBuildInfo micromap;
                micromap.description.type     = RHIAccelerationStructureType::eOpacityMicromap;
                RHIOpacityMicromapBuild& data = micromap.description.micromap;
                data.pDataBuffer     = Buffer(bytes, inputUsage, HeapVector<uint32_t>(bytes / sizeof(uint32_t), 0u).data());
                data.pTriangleBuffer = Buffer(sizeof(triangle), inputUsage, triangle);
                data.dataSize        = bytes;
                data.usages          = MakeVecView(&usage, 1);
                ASSERT_EQ(data.pDataBuffer->GetDeviceAddress() % 256u, 0u);
                micromap.pDestination = Structure(micromap.description);
                ASSERT_NE(micromap.pDestination, nullptr);
                RHIAccelerationStructureGeometry geometry;
                geometry.pVertexBuffer       = vertexBuffer;
                geometry.pIndexBuffer        = indexBuffer;
                geometry.vertexCount         = 3;
                geometry.vertexStride        = sizeof(Vec3);
                geometry.indexCount          = 3;
                geometry.opaque              = false;
                geometry.pOpacityMicromap    = micromap.pDestination;
                geometry.pOpacityIndexBuffer = mappingBuffer;
                RHIAccelerationStructureBuildInfo blas;
                blas.description.geometries = MakeVecView(&geometry, 1);
                blas.pDestination           = Structure(blas.description, MakeVecView(&micromap.pDestination, 1));
                ASSERT_NE(blas.pDestination, nullptr);
                RHIAccelerationStructureInstance instance;
                instance.pStructure = blas.pDestination;
                RHIAccelerationStructureBuildInfo tlas;
                tlas.description.type      = RHIAccelerationStructureType::eTopLevel;
                tlas.description.instances = MakeVecView(&instance, 1);
                tlas.pDestination          = Structure(tlas.description, MakeVecView(&blas.pDestination, 1));
                ASSERT_NE(tlas.pDestination, nullptr);
                for (RHIAccelerationStructureBuildInfo* build : {&micromap, &blas, &tlas})
                {
                    AllocateBuildScratch(*build);
                    ASSERT_NE(build->pScratchBuffer, nullptr);
                }
                RenderGraph& graph = *device->GetCurrentFrameRDG();
                ASSERT_TRUE(graph.Begin());
                for (RHIBuffer* buffer :
                     {cornerBuffer, materialBuffer, vertexBuffer, indexBuffer, mappingBuffer, data.pTriangleBuffer})
                {
                    graph.GetResourceManager()->ImportBuffer(buffer, RDGImportContents::eDefined);
                }
                graph.GetResourceManager()->ImportBuffer(data.pDataBuffer, RDGImportContents::eDefined);
                RDGComputePassDesc bake;
                bake.SetShaderProgramName("MicromapBakeSP");
                bake.BindStorageBuffer("MicromapTriangles", cornerBuffer);
                bake.BindStorageBuffer("MaterialBuffer", materialBuffer);
                bake.BindStorageBuffer("MicromapStates", data.pDataBuffer);
                bake.BindSeparateTexture("uTexture2DHeap", views);
                bake.BindSampler("uSamplerHeap", sampler);
                const uint32_t precision = device->GetGPUInfo().subTexelPrecisionBits;
                graph.AddComputePass(std::move(bake)).RecordPassCommands([level, precision, mode](RDGPassCmdEncoder& encoder) {
                    const uint32_t parameters[] = {0, level, 1, mode == 2 ? 2u : 3u, precision};
                    encoder.SetPushConstants(parameters);
                    encoder.Dispatch(((1u << level) + 7u) / 8u, ((1u << level) + 7u) / 8u, 1);
                });
                graph.AddTransferPass("BakedMicromap").BuildAccelerationStructure(micromap);
                graph.AddTransferPass("BakedMicromapBLAS").BuildAccelerationStructure(blas);
                graph.AddTransferPass("BakedMicromapTLAS").BuildAccelerationStructure(tlas);
                RDGComputePassDesc query;
                query.SetShaderProgramName("MicromapBakeQuerySP");
                query.BindAccelerationStructure("testScene", tlas.pDestination);
                query.BindStorageBuffer("MaterialBuffer", materialBuffer);
                query.BindSeparateTexture("uTexture2DHeap", views);
                query.BindSampler("uSamplerHeap", sampler);
                query.BindStorageBuffer("Results", result, RDGContentGuarantee::eFullWrite);
                graph.AddComputePass(std::move(query)).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
                    encoder.Dispatch(16, 16, 1);
                });
                graph.AddTransferPass("ReadBakedMicromapQueries")
                    .CopyBuffer(result, readback, {0, 0, 128 * 128 * sizeof(Vec4)});
                ASSERT_TRUE(graph.End()) << graph.GetResult().message;
                ASSERT_TRUE(device->ExecuteRenderGraph(graph));
                device->WaitForIdle();
                const Vec4* values = reinterpret_cast<const Vec4*>(readback->Map());
                ASSERT_NE(values, nullptr);
                uint32_t callbacks  = 0;
                uint32_t mismatches = 0;
                uint32_t samples    = 0;
                for (uint32_t pixel = 0; pixel < 128 * 128; ++pixel)
                {
                    mismatches += values[pixel].x != values[pixel].y ? 1u : 0u;
                    callbacks  += static_cast<uint32_t>(values[pixel].z);
                    samples    += static_cast<uint32_t>(values[pixel].w);
                }
                readback->Unmap();
                EXPECT_EQ(mismatches, 0u);
                EXPECT_LE(callbacks, previousCallbacks);
                if (level == 6)
                {
                    EXPECT_LT(callbacks, samples / (mode == 0 ? 8 : 2));
                }
                previousCallbacks = callbacks;
                device->NextFrame();
            }
        }
    }
    else
    {
        GTEST_SKIP() << "Four-state opacity micromaps unavailable";
    }
}
