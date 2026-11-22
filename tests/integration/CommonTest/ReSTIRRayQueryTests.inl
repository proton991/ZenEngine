class ReSTIRRayQueryTest : public RayQueryIntegrationTest
{
protected:
    RHIBuffer* ReadSignal(RenderGraph& graph, RHITexture* texture, RHISampler* sampler)
    {
        RHIFlags<RHIBufferUsageFlagBits> usage;
        usage.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer);
        RHIBuffer* output = Buffer(8 * sizeof(Vec4), usage, nullptr, RHIBufferAllocateType::eGPU);
        RHIBuffer* readback =
            Buffer(8 * sizeof(Vec4), RHIFlags<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferDstBuffer), nullptr,
                   RHIBufferAllocateType::eCPURead);
        RDGComputePassDesc pass;
        pass.SetShaderProgramName("HybridTextureCheckSP");
        pass.BindSampledTexture("source", sampler, texture->GetDefaultView());
        pass.BindStorageBuffer("Results", output, RDGContentGuarantee::eFullWrite);
        graph.AddComputePass(std::move(pass)).RecordPassCommands([](RDGPassCmdEncoder& encoder) { encoder.Dispatch(1, 1, 1); });
        graph.AddTransferPass("ReadReSTIRSignal").CopyBuffer(output, readback, {0, 0, 8 * sizeof(Vec4)}).NeverCull();
        return readback;
    }

    void CheckLightOccluders(SceneRayQuery& queries, RenderScene& scene)
    {
        ShaderProgramManager::GetInstance().StoreProgram(
            ZEN_NEW() ComputeFileSP(device, "LightOccludersSP", "ShadowMapping/light_occluders.comp.spv"));
        ShaderProgramManager::GetInstance().StoreProgram(
            ZEN_NEW() ComputeFileSP(device, "LightOccluderCheckSP", "VoxelGI/Calibration/light_occluder_check.comp.spv"));
        SceneUniformData lighting;
        lighting.lightInfo.x               = 1;
        lighting.lights[0].directionType.w = 1;
        lighting.lights[0].colorIntensity  = Vec4(1);
        lighting.lights[0].coneShadow.z    = 1;
        VoxelGIUniformData gi;
        gi.gridMinVoxelSize.w = 1;
        HeapVector<glm::uvec4> queryResults(3 * 4096);
        for (uint32_t scenario = 0; scenario < (SupportsHybridRayTracingPipeline(device->GetGPUInfo()) ? 6u : 3u); ++scenario)
        {
            SCOPED_TRACE(scenario);
            const uint32_t frame             = scenario % 3;
            const bool     pipeline          = scenario >= 3;
            lighting.lights[0].positionRange = Vec4(frame == 1 ? 0.7f : 0.0f, 0, 0, 0);
            Mat4 transform(1);
            transform[3].z = frame == 2 ? .5f : 0;
            ASSERT_TRUE(scene.SetInstanceTransform(0, transform));
            ASSERT_TRUE(scene.CommitGeometryUpdates());
            ASSERT_TRUE(scene.Update());
            RenderGraph& graph = *device->GetCurrentFrameRDG();
            ASSERT_TRUE(graph.Begin());
            ASSERT_TRUE(queries.BuildRenderGraph(scene));
            RHIBuffer* cache = Buffer(MaxSceneLights * 6u * 16u * 16u * 3u * sizeof(Vec4),
                                      RHIFlags<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eStorageBuffer), nullptr,
                                      RHIBufferAllocateType::eGPU);
            RHIFlags<RHIBufferUsageFlagBits> outputUsage;
            outputUsage.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer);
            RHIBuffer* output = Buffer(4096 * sizeof(glm::uvec4), outputUsage, nullptr, RHIBufferAllocateType::eGPU);
            RHIBuffer* readback =
                Buffer(4096 * sizeof(glm::uvec4), RHIFlags<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferDstBuffer),
                       nullptr, RHIBufferAllocateType::eCPURead);
            RDGComputePassDesc    compute;
            RDGRayTracingPassDesc rays;
            RDGDispatchPassDesc&  build =
                pipeline ? static_cast<RDGDispatchPassDesc&>(rays) : static_cast<RDGDispatchPassDesc&>(compute);
            build.SetShaderProgramName(pipeline ? "LightOccludersPipelineSP" : "LightOccludersSP");
            build.SetPassTag("BuildTestOccluders");
            queries.BindInputs(build, scene);
            build.BindValue("uGISettings", gi);
            build.BindValue("uSceneData", lighting);
            build.BindStorageBuffer("LightOccluders", cache, RDGContentGuarantee::eFullWrite);
            if (pipeline)
            {
                rays.SetShaderGroups({{RHIRayTracingShaderGroupType::eRayGeneration, 0},
                                      {RHIRayTracingShaderGroupType::eMiss, 1},
                                      {RHIRayTracingShaderGroupType::eHit, kUnusedRayTracingShader, 2, 3}});
                graph.AddRayTracingPass(std::move(rays)).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
                    encoder.TraceRays(16, 16, MaxSceneLights * 6);
                });
            }
            else
            {
                graph.AddComputePass(std::move(compute)).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
                    encoder.Dispatch(2, 2, MaxSceneLights * 6);
                });
            }
            RDGComputePassDesc check;
            check.SetShaderProgramName("LightOccluderCheckSP");
            check.SetPassTag("CheckTestOccluders");
            queries.BindInputs(check, scene);
            check.BindValue("uGISettings", gi);
            check.BindValue("uSceneData", lighting);
            check.BindStorageBuffer("LightOccluders", cache);
            check.BindStorageBuffer("Results", output, RDGContentGuarantee::eFullWrite);
            graph.AddComputePass(std::move(check)).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
                encoder.Dispatch(64, 1, 1);
            });
            graph.AddTransferPass("ReadTestOccluders")
                .CopyBuffer(output, readback, {0, 0, 4096 * sizeof(glm::uvec4)})
                .NeverCull();
            ASSERT_TRUE(graph.End()) << graph.GetResult().message;
            ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
            queries.OnRenderGraphExecuted(RenderFrameResult::eSubmitted);
            device->FlushRHIThread();
            device->WaitForIdle();
            const glm::uvec4* result = reinterpret_cast<const glm::uvec4*>(readback->Map());
            ASSERT_NE(result, nullptr);
            if (pipeline)
            {
                EXPECT_EQ(std::memcmp(result, queryResults.data() + frame * 4096, 4096 * sizeof(glm::uvec4)), 0);
            }
            else
            {
                std::memcpy(queryResults.data() + frame * 4096, result, 4096 * sizeof(glm::uvec4));
            }
            uint32_t cached = 0;
            for (uint32_t index = 0; index < 4096; ++index)
            {
                EXPECT_EQ(result[index].z, 0u) << "segment " << index;
                cached += result[index].x;
            }
            EXPECT_GT(cached, 50u);
            readback->Unmap();
            device->NextFrame();
        }
    }

    RHITexture* Texture(RenderGraph& graph, DataFormat format, const void* data, uint32_t bytes)
    {
        RHITextureCreateInfo info;
        info.type   = RHITextureType::e2D;
        info.width  = 8;
        info.height = info.depth = 1;
        info.format              = format;
        info.usageFlags.SetFlags(RHITextureUsageFlagBits::eSampled, RHITextureUsageFlagBits::eStorage,
                                 RHITextureUsageFlagBits::eTransferDst);
        RHITexture* texture = device->CreateTexture(info);
        textures.push_back(texture);
        if (data != nullptr)
        {
            RHIFlags<RHIBufferUsageFlagBits> usage;
            usage.SetFlag(RHIBufferUsageFlagBits::eTransferSrcBuffer);
            RHIBuffer* upload = Buffer(bytes, usage, data);
            graph.GetResourceManager()->ImportHostWrittenBuffer(upload);
            RHIBufferTextureCopyRegion region;
            region.textureSize = RHIOffset3D(8, 1, 1);
            region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
            graph.AddTransferPass("UploadReSTIRReceiver").CopyBufferToTexture(upload, texture, region);
        }
        else
        {
            graph.AddTransferPass("ClearReSTIRObservation").ClearTexture(texture, Color(0));
        }
        return texture;
    }
};

TEST_P(ReSTIRRayQueryTest, TemporalAndCheckerboardReuseRejectForeignSurfacesResetAndOcclusion)
{
    if (device->GetGPUInfo().rayQuery.IsUsable())
    {
        ShaderProgramManager::GetInstance().StoreProgram(
            ZEN_NEW() ComputeFileSP(device, "HybridTextureCheckSP", "VoxelGI/Calibration/hybrid_texture_check.comp.spv"));
        ShaderProgramManager::GetInstance().BuildShaderPrograms(device);
        sg::Scene source;
        source.GetAABB()                 = sg::AABB(Vec3(-2), Vec3(2));
        UniquePtr<sg::Material> material = MakeUnique<sg::Material>("opaque");
        material->index                  = 0;
        material->data.baseColorFactor   = Vec4(1);
        UniquePtr<sg::SubMesh> primitive = MakeUnique<sg::SubMesh>("visibility occluder", 0, 3, 3);
        primitive->SetMaterial(0, material.Get());
        UniquePtr<sg::Mesh> mesh = MakeUnique<sg::Mesh>("visibility occluder");
        mesh->SetAABB(Vec3(-1, -1, 0.5f), Vec3(1, 1, 0.5f));
        mesh->AddSubMesh(primitive.Get());
        HeapVector<UniquePtr<sg::Node>> nodes;
        UniquePtr<sg::Node>             node = MakeUnique<sg::Node>(0, "visibility occluder");
        node->SetData(0, Mat4(1));
        node->AddComponent(mesh.Get());
        source.AddRenderableNode(node.Get());
        nodes.push_back(std::move(node));
        source.SetNodes(std::move(nodes));
        source.AddComponent(std::move(primitive));
        source.AddComponent(std::move(mesh));
        source.AddComponent(std::move(material));
        asset::Vertex vertices[3]{};
        for (uint32_t i = 0; i < 3; ++i)
        {
            vertices[i].pos    = Vec4(i == 0 ? -1 : i == 1 ? 1 : 0, i == 2 ? 1 : -1, 0.5f, 1);
            vertices[i].normal = Vec4(0, 0, 1, 0);
            vertices[i].color  = Vec4(1);
        }
        uint32_t   indices[] = {0, 1, 2};
        sg::Camera camera(Vec3(0, 0, 3), Vec3(0), 1);
        SceneData  data{};
        data.pScene      = &source;
        data.pVertices   = vertices;
        data.numVertices = 3;
        data.pIndices    = indices;
        data.numIndices  = 3;
        data.pCamera     = &camera;
        RenderScene scene(device, data);
        scene.LoadSceneMaterials();
        scene.PrepareBuffers();
        ASSERT_TRUE(scene.Update());
        SceneRayQuery                 queries(device);
        RHISampler*                   sampler   = device->CreateSampler({});
        constexpr float               pi        = 3.14159265358979323846f;
        constexpr uint32_t            packedOne = 256u | (256u << 9u) | (256u << 18u) | (16u << 27u);
        HeapVector<PackedGIReservoir> queryResults(8 * 8);
        uint32_t                      maxWeightULP = 0;
        for (uint32_t scenario = 0; scenario < (SupportsHybridRayTracingPipeline(device->GetGPUInfo()) ? 16u : 8u); ++scenario)
        {
            SCOPED_TRACE(scenario);
            const uint32_t frame      = scenario % 8;
            const bool     pipeline   = scenario >= 8;
            const bool     reflection = frame >= 5;
            const bool     spatial    = frame == 3 || frame >= 6;
            RenderGraph&   graph      = *device->GetCurrentFrameRDG();
            ASSERT_TRUE(graph.Begin());
            ASSERT_TRUE(queries.BuildRenderGraph(scene));
            HeapVector<float>      depth(8, 0.5f), integral(8, 1.0f);
            HeapVector<Vec4>       positions(8, Vec4(0, 0, 0, 1)), oldPositions   = positions;
            HeapVector<Vec4>       normals(8, Vec4(0.5f, 0.5f, 0, 0)), oldNormals = normals;
            HeapVector<Vec4>       roughness(8, Vec4(0, 0.5f, 0, 0));
            HeapVector<Vec2>       motion(8, Vec2(0));
            HeapVector<Vec4>       lengths(8, Vec4(0, 0, 0, 0.5f));
            HeapVector<glm::uvec2> receivers(8, glm::uvec2(0x80008000u, 1)), oldReceivers = receivers;
            receivers[1].y    = 2;
            oldPositions[2].z = 0.1f;
            oldNormals[3]     = Vec4(0);
            positions[4].w    = 0;
            depth[5]          = 1;
            receivers[6].y    = 0;
            // At normal incidence and roughness 0.5, this unit-integral fixture's
            // GGX density is 4/pi. Normalize the input estimates to Lo in both cases.
            const float                   density = reflection ? 4.0f / pi : 1.0f / pi;
            const PackedGIReservoir       fresh{0, 0, 1, 0x81ffffffu, packedOne, 1.0f / density};
            const PackedGIReservoir       previous{0, 0, 1, 0xe4ffffffu, packedOne + (1u << 27u), 1.0f / density};
            HeapVector<PackedGIReservoir> current(8, fresh), history(8, previous), empty(8, PackedGIReservoir{});
            if (spatial)
            {
                // Four identical neighbors and one center: uniform pairwise MIS gives
                // the center 1/5 of the estimate, rather than 1/2.
                current[4].radiance += 1u << 27u;
            }
            if (frame == 4)
            {
                oldPositions[7].x = 0.1f;
                current[7].weight = 0.0f;
            }
            RHIFlags<RHIBufferUsageFlagBits> usage;
            usage.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer);
            RHIBuffer* initial      = Buffer(8 * sizeof(fresh), usage, current.data());
            RHIBuffer* old          = Buffer(8 * sizeof(fresh), usage, history.data());
            RHIBuffer* noReflection = Buffer(8 * sizeof(fresh), usage, empty.data());
            RHIBuffer* output =
                spatial && !reflection ? initial : Buffer(8 * sizeof(fresh), usage, nullptr, RHIBufferAllocateType::eGPU);
            RHIBuffer* specularOutput =
                spatial && reflection ? initial : Buffer(8 * sizeof(fresh), usage, nullptr, RHIBufferAllocateType::eGPU);
            RHIBuffer* readback =
                Buffer(8 * sizeof(fresh), RHIFlags<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferDstBuffer), nullptr,
                       RHIBufferAllocateType::eCPURead);
            graph.GetResourceManager()->ImportHostWrittenBuffer(initial);
            graph.GetResourceManager()->ImportHostWrittenBuffer(old);
            graph.GetResourceManager()->ImportHostWrittenBuffer(noReflection);
            // Temporal reuse packs every texel's receiver for spatial reuse; the spatial cases start from that packing.
            HeapVector<glm::uvec4> guides(16);
            for (uint32_t pixel = 0; pixel < 8; ++pixel)
            {
                guides[2 * pixel] =
                    glm::uvec4(glm::floatBitsToUint(positions[pixel].x), glm::floatBitsToUint(positions[pixel].y),
                               glm::floatBitsToUint(positions[pixel].z), glm::packUnorm2x16(Vec2(normals[pixel])));
                guides[2 * pixel + 1] = glm::uvec4(receivers[pixel].x, depth[pixel] < 1.0f ? receivers[pixel].y : 0u,
                                                   glm::floatBitsToUint(roughness[pixel].y), 0u);
            }
            RHIBuffer* receiverGuides = Buffer(16 * sizeof(glm::uvec4), usage, guides.data());
            graph.GetResourceManager()->ImportHostWrittenBuffer(receiverGuides);
            RHITexture* inputs[] = {Texture(graph, DataFormat::eR32SFloat, depth.data(), 8 * sizeof(float)),
                                    Texture(graph, DataFormat::eR32G32B32A32SFloat, normals.data(), 8 * sizeof(Vec4)),
                                    Texture(graph, DataFormat::eR32G32B32A32SFloat, roughness.data(), 8 * sizeof(Vec4)),
                                    Texture(graph, DataFormat::eR32G32UInt, receivers.data(), 8 * sizeof(glm::uvec2)),
                                    Texture(graph, DataFormat::eR32G32SFloat, motion.data(), 8 * sizeof(Vec2)),
                                    Texture(graph, DataFormat::eR32G32B32A32SFloat, positions.data(), 8 * sizeof(Vec4)),
                                    Texture(graph, DataFormat::eR32G32B32A32SFloat, oldPositions.data(), 8 * sizeof(Vec4)),
                                    Texture(graph, DataFormat::eR32G32B32A32SFloat, oldNormals.data(), 8 * sizeof(Vec4)),
                                    Texture(graph, DataFormat::eR32G32UInt, oldReceivers.data(), 8 * sizeof(glm::uvec2)),
                                    Texture(graph, DataFormat::eR32G32B32A32SFloat, lengths.data(), 8 * sizeof(Vec4)),
                                    Texture(graph, DataFormat::eR32SFloat, integral.data(), 8 * sizeof(float))};
            RHITexture* bounce   = Texture(graph, DataFormat::eR16G16B16A16SFloat, nullptr, 0);
            RHITexture* specular = Texture(graph, DataFormat::eR16G16B16A16SFloat, nullptr, 0);
            HeapVector<float> distances(8, 0.125f);
            HeapVector<Vec2>  reflectionGuides(8, Vec2(0.25f, 0.5f));
            RHITexture*       diffuseDistance = Texture(graph, DataFormat::eR32SFloat, distances.data(), 8 * sizeof(float));
            RHITexture* reflectionGuide = Texture(graph, DataFormat::eR32G32SFloat, reflectionGuides.data(), 8 * sizeof(Vec2));
            const char* names[]         = {"receiverDepth",    "receiverNormal",   "receiverRoughness", "receiverSurface",
                                           "receiverMotion",   "receiverPosition", "previousPosition",  "previousNormal",
                                           "previousReceiver", "previousLength",   "restirLobeIntegral"};
            HybridGIUniformData hybrid;
            hybrid.sampling             = glm::uvec4(frame, 1, 32, 1);
            hybrid.bounce               = glm::uvec4(1, frame == 1 || frame == 4 ? 1 : 0, 0, 0);
            hybrid.reflection           = glm::uvec4(reflection ? 1 : 0, reflection ? 1 : 0, 0, 0);
            hybrid.rejection            = Vec4(0.001f, 1, 1, 0);
            hybrid.reservoir            = glm::uvec4(reflection ? 2 : 1, 20, 7, frame == 7 ? 0 : 1);
            hybrid.previousViewPosition = Vec4(0, 0, 3, 1);
            VoxelGIUniformData gi;
            for (uint32_t stage = 0; stage < (spatial ? 2u : 1u); ++stage)
            {
                RDGComputePassDesc    compute;
                RDGRayTracingPassDesc rays;
                RDGDispatchPassDesc&  pass =
                    pipeline ? static_cast<RDGDispatchPassDesc&>(rays) : static_cast<RDGDispatchPassDesc&>(compute);
                // Spatial reuse runs one signal per dispatch; each case allocates one signal.
                const char* spatialProgram = reflection
                                               ? (pipeline ? "ReSTIRSpatialReflectionPipelineSP" : "ReSTIRSpatialReflectionSP")
                                               : (pipeline ? "ReSTIRSpatialDiffusePipelineSP" : "ReSTIRSpatialDiffuseSP");
                pass.SetShaderProgramName(spatial    ? spatialProgram
                                          : pipeline ? "ReSTIRTemporalPipelineSP"
                                                     : "ReSTIRTemporalSP");
                pass.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
                pass.BindValue("uHybridData", hybrid);
                pass.BindValue("uGISettings", gi);
                pass.BindValue("uSceneData", scene.GetSceneUniformData(), sizeof(SceneUniformData));
                ReSTIRUniformData restirData;
                restirData.options = glm::uvec4(spatial ? 1 : 0, reflection ? 1 : 0, reflection ? 0 : 1, stage + 1);
                pass.BindValue("uReSTIRData", restirData);
                queries.BindInputs(pass, scene);
                for (uint32_t i = 0; i < std::size(inputs); ++i)
                {
                    pass.BindSampledTexture(names[i], sampler, inputs[i]->GetDefaultView());
                }
                pass.BindStorageBuffer("GIReservoirCurrent", initial);
                pass.BindStorageBuffer("GIReservoirPrevious", old);
                pass.BindStorageBuffer("GIReservoirOutput", output);
                pass.BindStorageBuffer("ReflectionReservoirCurrent", reflection ? initial : noReflection);
                pass.BindStorageBuffer("ReflectionReservoirPrevious", reflection ? old : noReflection);
                pass.BindStorageBuffer("ReflectionReservoirOutput", specularOutput, RDGContentGuarantee::eFullWrite);
                pass.BindStorageBuffer("GIReceiverGuides", receiverGuides);
                pass.BindStorageImage("rawBounce", bounce->GetDefaultView());
                pass.BindStorageImage("rawSpecular", specular->GetDefaultView());
                pass.BindStorageImage("rawDiffuseDistance", diffuseDistance->GetDefaultView());
                pass.BindStorageImage("rawReflection", reflectionGuide->GetDefaultView());
                if (pipeline)
                {
                    rays.SetShaderGroups(
                        {{RHIRayTracingShaderGroupType::eRayGeneration, 0},
                         {RHIRayTracingShaderGroupType::eMiss, 1},
                         {RHIRayTracingShaderGroupType::eHit, kUnusedRayTracingShader, kUnusedRayTracingShader, 2}});
                    graph.AddRayTracingPass(std::move(rays)).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
                        encoder.TraceRays(8, 1, 1);
                    });
                }
                else
                {
                    graph.AddComputePass(std::move(compute)).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
                        encoder.Dispatch(1, 1, 1);
                    });
                }
            }
            graph.AddTransferPass("ReadReSTIRTemporal")
                .CopyBuffer(reflection ? specularOutput : output, readback, {0, 0, 8 * sizeof(fresh)})
                .NeverCull();
            RHIBuffer* distanceReadbacks[] = {ReadSignal(graph, diffuseDistance, sampler),
                                              ReadSignal(graph, reflectionGuide, sampler)};
            ASSERT_TRUE(graph.End()) << graph.GetResult().message;
            ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
            queries.OnRenderGraphExecuted(RenderFrameResult::eSubmitted);
            device->FlushRHIThread();
            device->WaitForIdle();
            const PackedGIReservoir* result = reinterpret_cast<const PackedGIReservoir*>(readback->Map());
            ASSERT_NE(result, nullptr);
            if (!pipeline)
            {
                std::memcpy(queryResults.data() + frame * 8, result, 8 * sizeof(fresh));
            }
            for (uint32_t i = 0; i < 8; ++i)
            {
                SCOPED_TRACE(i);
                if (pipeline)
                {
                    const PackedGIReservoir& expectedReservoir = queryResults[frame * 8 + i];
                    EXPECT_EQ(result[i].x, expectedReservoir.x);
                    EXPECT_EQ(result[i].y, expectedReservoir.y);
                    EXPECT_EQ(result[i].z, expectedReservoir.z);
                    EXPECT_EQ(result[i].normalCount, expectedReservoir.normalCount);
                    EXPECT_EQ(result[i].radiance, expectedReservoir.radiance);
                    // Different shader stages can round the floating-point MIS weight differently.
                    EXPECT_FLOAT_EQ(result[i].weight, expectedReservoir.weight);
                    const uint32_t actualBits   = std::bit_cast<uint32_t>(result[i].weight);
                    const uint32_t expectedBits = std::bit_cast<uint32_t>(expectedReservoir.weight);
                    maxWeightULP =
                        std::max(maxWeightULP, std::max(actualBits, expectedBits) - std::min(actualBits, expectedBits));
                }
                const bool     valid        = i != 5 && i != 6;
                const bool     reused       = (frame == 1 || frame == 4 || frame == 5) && (i == 0 || i == 7);
                const uint32_t historyCount = reflection ? 7u : 20u;
                const uint32_t count        = spatial && (i == 2 || i == 3) ? 5u : reused ? historyCount + 1u : 1u;
                EXPECT_EQ((result[i].normalCount >> 24u) & 127u, valid ? count : 0u);
                const float radiance = float(result[i].radiance & 511u) * std::exp2(float(int(result[i].radiance >> 27u) - 24));
                const float expected = !valid || (frame == 4 && i == 7) ? 0.0f
                                     : spatial && i == 3                ? 1.8f
                                     : spatial && i == 4                ? 2.0f
                                     : reused ? float(2u * historyCount + 1u) / float(historyCount + 1u)
                                              : 1.0f;
                EXPECT_NEAR(radiance * result[i].weight * density, expected, 1e-4f);
            }
            readback->Unmap();
            for (uint32_t signal = 0; signal < 2; ++signal)
            {
                const Vec4* guides = reinterpret_cast<const Vec4*>(distanceReadbacks[signal]->Map());
                ASSERT_NE(guides, nullptr);
                for (uint32_t i = 0; i < 8; ++i)
                {
                    const bool updated = spatial && frame != 7 && reflection == (signal == 1) && i != 5 && i != 6;
                    EXPECT_FLOAT_EQ(guides[i].x, updated ? 1.0f : signal == 0 ? 0.125f : 0.25f);
                    if (signal == 1)
                    {
                        EXPECT_FLOAT_EQ(guides[i].y, 0.5f);
                    }
                }
                distanceReadbacks[signal]->Unmap();
            }
            device->NextFrame();
        }
        RecordProperty("max_rt_weight_ulp", maxWeightULP);
        CheckLightOccluders(queries, scene);
        queries.Destroy();
        scene.Destroy();
    }
    else
    {
        GTEST_SKIP() << "Ray queries unavailable on this device";
    }
}
INSTANTIATE_TEST_SUITE_P(SubmissionModes, ReSTIRRayQueryTest, testing::Values(0u, 1u, 2u, 3u));
