TEST_P(RayQueryIntegrationTest, HitLightingUsesActualMaterialsAndRTVisibilityOutsideTheVoxelCache)
{
    if (device->GetGPUInfo().rayQuery.IsUsable())
    {
        ShaderProgramManager::GetInstance().StoreProgram(
            ZEN_NEW() ComputeFileSP(device, "RayHitLightingCheckSP", "VoxelGI/Calibration/ray_hit_lighting_check.comp.spv"));
        if (SupportsHybridRayTracingPipeline(device->GetGPUInfo()))
        {
            const HeapVector<ShaderProgram::StageFile> modules = {
                {RHIShaderStage::eRayGeneration, "VoxelGI/Calibration/ray_hit_lighting_check.rgen.spv"},
                {RHIShaderStage::eMiss, "VoxelGI/ray_radiance.rmiss.spv"},
                {RHIShaderStage::eMiss, "VoxelGI/ray_shadow.rmiss.spv"},
                {RHIShaderStage::eClosestHit, "VoxelGI/hybrid_hit.rchit.spv"},
                {RHIShaderStage::eAnyHit, "VoxelGI/ray_alpha_radiance.rahit.spv"},
                {RHIShaderStage::eAnyHit, "VoxelGI/ray_alpha.rahit.spv"}};
            ShaderProgramManager::GetInstance().StoreProgram(ZEN_NEW()
                                                                 RayTracingFileSP(device, "RayHitPipelineCheckSP", modules));
        }
        sg::Scene source;
        source.GetAABB() = sg::AABB(Vec3(-1), Vec3(3, 1, 1));
        asset::Vertex                   vertices[9]{};
        uint32_t                        indices[9];
        HeapVector<UniquePtr<sg::Node>> nodes;
        const Vec3                      colors[] = {Vec3(1, 0.25f, 0), Vec3(0, 0.5f, 1), Vec3(1)};
        for (uint32_t part = 0; part < 3; ++part)
        {
            const float center = part == 2 ? 0.5f : float(part) * 2.0f;
            const float radius = part == 2 ? 0.25f : 0.5f;
            for (uint32_t corner = 0; corner < 3; ++corner)
            {
                const uint32_t index   = part * 3 + corner;
                vertices[index].pos    = Vec4(center
                                                  + (corner == 0   ? -radius
                                                     : corner == 1 ? radius
                                                                   : 0),
                                              corner == 2 ? radius : -radius, part == 2 ? 0.5f : 0.0f, 1);
                vertices[index].normal = Vec4(0, 0, 1, 0);
                vertices[index].color  = Vec4(1);
                indices[index]         = index;
            }
            UniquePtr<sg::Material> material    = MakeUnique<sg::Material>("hit material");
            material->index                     = part;
            material->data.baseColorFactor      = Vec4(colors[part], 1);
            material->data.metallicFactor       = 0;
            material->data.materialProperties.z = 1;
            UniquePtr<sg::Mesh> mesh            = MakeUnique<sg::Mesh>("hit triangle");
            mesh->SetAABB(Vec3(center - radius, -radius, 0), Vec3(center + radius, radius, 0.5f));
            UniquePtr<sg::SubMesh> primitive = MakeUnique<sg::SubMesh>("surface", part * 3, 3, 3);
            primitive->SetMaterial(part, material.Get());
            mesh->AddSubMesh(primitive.Get());
            UniquePtr<sg::Node> node = MakeUnique<sg::Node>(part, "surface instance");
            node->SetData(part, Mat4(1));
            node->AddComponent(mesh.Get());
            source.AddRenderableNode(node.Get());
            nodes.push_back(std::move(node));
            source.AddComponent(std::move(primitive));
            source.AddComponent(std::move(mesh));
            source.AddComponent(std::move(material));
        }
        source.SetNodes(std::move(nodes));
        sg::Camera camera(Vec3(0, 0, 0.25f), Vec3(0), 1);
        SceneData  data{};
        data.pScene      = &source;
        data.pVertices   = vertices;
        data.numVertices = 9;
        data.pIndices    = indices;
        data.numIndices  = 9;
        data.pCamera     = &camera;
        asset::PrepareScenePlacement(source, asset::ScenePlacement::PreserveAuthored);
        RenderScene scene(device, data);
        scene.LoadSceneMaterials();
        scene.PrepareBuffers();
        ASSERT_TRUE(scene.SetVoxelBounds(sg::AABB(Vec3(10), Vec3(11))));
        SceneRayQuery queries(device);

        RHITextureCreateInfo info;
        info.type  = RHITextureType::eCube;
        info.width = info.height = 1;
        info.arrayLayers         = 6;
        info.format              = DataFormat::eR32G32B32A32SFloat;
        info.usageFlags.SetFlags(RHITextureUsageFlagBits::eSampled, RHITextureUsageFlagBits::eTransferDst);
        RHITexture* environment = device->CreateTexture(info);
        textures.push_back(environment);
        const Vec3 sky(0.25f, 0.5f, 0.75f);
        const Vec4 pixels[] = {Vec4(sky, 1), Vec4(sky, 1), Vec4(sky, 1), Vec4(sky, 1), Vec4(sky, 1), Vec4(sky, 1)};
        RHIBuffer* upload =
            Buffer(sizeof(pixels), RHIFlags<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferSrcBuffer), pixels);
        HeapVector<float> zeros(ZEN_ENVIRONMENT_IMPORTANCE_ROWS + 1, 0);
        RHIBuffer*        distribution =
            Buffer(static_cast<uint32_t>(zeros.size() * sizeof(float)),
                   RHIFlags<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eStorageBuffer), zeros.data());
        RHIFlags<RHIBufferUsageFlagBits> outputUsage;
        outputUsage.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer);
        RHIBuffer* output = Buffer(4 * sizeof(Vec4), outputUsage, nullptr, RHIBufferAllocateType::eGPU);
        RHIBuffer* readback =
            Buffer(4 * sizeof(Vec4), RHIFlags<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferDstBuffer), nullptr,
                   RHIBufferAllocateType::eCPURead);
        RHISampler*      sampler = device->CreateSampler({});
        HeapVector<Vec4> queryResults(12);
        for (uint32_t iteration = 0; iteration < (SupportsHybridRayTracingPipeline(device->GetGPUInfo()) ? 12u : 6u);
             ++iteration)
        {
            SCOPED_TRACE(iteration);
            const uint32_t scenario = iteration % 6;
            const bool     pipeline = iteration >= 6;
            ASSERT_TRUE(scene.SetInstanceEnabled(2, scenario == 1 || scenario == 2 || scenario == 4));
            const Mat4 transform = scenario == 2 ? glm::translate(Mat4(1), Vec3(-0.5f, 0, 0))
                                 : scenario == 4
                                     ? glm::translate(Mat4(1), Vec3(0.5f, 0, 0.5f)) * glm::scale(Mat4(1), Vec3(2048, 2048, 1))
                                           * glm::translate(Mat4(1), Vec3(-0.5f, 0, -0.5f))
                                     : Mat4(1);
            ASSERT_TRUE(scene.SetInstanceTransform(2, transform));
            {
                sg::MaterialData material = source.GetComponents<sg::Material>()[0]->data;
                material.baseColorFactor  = Vec4(scenario == 5 ? Vec3(0, 0, 1) : colors[0], 1);
                ASSERT_TRUE(scene.UpdateMaterial(0, material));
            }
            ASSERT_TRUE(scene.Update());
            RenderGraph& graph = *device->GetCurrentFrameRDG();
            ASSERT_TRUE(graph.Begin());
            ASSERT_TRUE(queries.BuildRenderGraph(scene));
            if (scenario == 0)
            {
                for (uint32_t face = 0; face < 6; ++face)
                {
                    RHIBufferTextureCopyRegion region;
                    region.bufferOffset = face * sizeof(Vec4);
                    region.textureSize  = RHIOffset3D(1);
                    region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
                    region.textureSubresources.baseArrayLayer = face;
                    graph.AddTransferPass("UploadHitSky").CopyBufferToTexture(upload, environment, region);
                }
            }
            VoxelGIUniformData gi{};
            gi.gridMinVoxelSize = Vec4(10, 10, 10, 0.01f);
            gi.volume           = Vec4(1, 1, 1, 1);
            gi.limits.z         = 1;
            gi.lighting         = Vec4(scenario < 3 ? 1 : 0, scenario >= 3 ? 1 : 0, 0, 1);
            SceneUniformData lighting{};
            lighting.environment              = Vec4(1, 0, 1, 0);
            lighting.environmentOrientation   = Vec4(0, 0, 0, 1);
            lighting.lightInfo.x              = 1;
            lighting.lights[0].positionRange  = Vec4(1, 0, 1, 0);
            lighting.lights[0].directionType  = Vec4(0, 0, -1, scenario == 2 ? 0 : 1);
            lighting.lights[0].colorIntensity = Vec4(Vec3(1), glm::pi<float>() * (scenario == 2 ? 1 : std::sqrt(8.0f)));
            lighting.lights[0].coneShadow.z   = 1;
            RDGComputePassDesc    compute;
            RDGRayTracingPassDesc rays;
            RDGDispatchPassDesc&  pass =
                pipeline ? static_cast<RDGDispatchPassDesc&>(rays) : static_cast<RDGDispatchPassDesc&>(compute);
            pass.SetShaderProgramName(pipeline ? "RayHitPipelineCheckSP" : "RayHitLightingCheckSP");
            pass.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
            pass.BindValue("uGISettings", gi);
            pass.BindValue("uSceneData", lighting);
            queries.BindInputs(pass, scene);
            for (const char* name : {"EnvironmentRows", "EnvironmentColumns", "EnvironmentLuminance", "EnvironmentTiles",
                                     "EnvironmentSources", "EnvironmentResidualRows", "EnvironmentResidualColumns"})
            {
                pass.BindStorageBuffer(name, distribution);
            }
            pass.BindSampledTexture("coneEnvironmentMap", sampler, environment->GetDefaultView());
            pass.BindStorageBuffer("Results", output, RDGContentGuarantee::eFullWrite);
            if (pipeline)
            {
                rays.SetShaderGroups(
                    {{RHIRayTracingShaderGroupType::eRayGeneration, 0},
                     {RHIRayTracingShaderGroupType::eMiss, 1},
                     {RHIRayTracingShaderGroupType::eMiss, 2},
                     {RHIRayTracingShaderGroupType::eHit, kUnusedRayTracingShader, 3, 4},
                     {RHIRayTracingShaderGroupType::eHit, kUnusedRayTracingShader, kUnusedRayTracingShader, 5}},
                    2);
                graph.AddRayTracingPass(std::move(rays)).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
                    encoder.TraceRays(2, 1, 1);
                });
            }
            else
            {
                graph.AddComputePass(std::move(compute)).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
                    encoder.Dispatch(2, 1, 1);
                });
            }
            graph.AddTransferPass("ReadHitLighting").CopyBuffer(output, readback, {0, 0, 4 * sizeof(Vec4)}).NeverCull();
            ASSERT_TRUE(graph.End()) << graph.GetResult().message;
            const bool succeeded = device->ExecuteRenderGraph(graph);
            queries.OnRenderGraphExecuted(succeeded ? RenderFrameResult::eSubmitted : RenderFrameResult::eFailed);
            ASSERT_TRUE(succeeded);
            device->WaitForIdle();
            const Vec4* values = reinterpret_cast<const Vec4*>(readback->Map());
            ASSERT_NE(values, nullptr);
            for (uint32_t receiver = 0; receiver < 2; ++receiver)
            {
                const Vec3 color    = scenario == 5 && receiver == 0 ? Vec3(0, 0, 1) : colors[receiver];
                const bool blocked  = scenario == 4 || ((scenario == 1 || scenario == 2) && receiver == 0);
                const Vec3 expected = blocked ? Vec3(0) : color * 0.96f * (scenario >= 3 ? sky : Vec3(1));
                for (uint32_t component = 0; component < 3; ++component)
                {
                    EXPECT_NEAR(values[receiver][component], expected[component], 2e-4f);
                    if (pipeline)
                    {
                        EXPECT_FLOAT_EQ(values[receiver][component], queryResults[scenario * 2 + receiver][component]);
                    }
                }
                EXPECT_EQ(values[receiver].w, float(receiver));
                // Visibility rays after a shaded primary must retain their independent payload.
                EXPECT_EQ(values[receiver + 2u],
                          Vec4(1, scenario == 4 || (scenario == 2 && receiver == 0) ? 1.0f : 0.0f, 0, 1));
                if (!pipeline)
                {
                    queryResults[scenario * 2 + receiver] = values[receiver];
                }
            }
            readback->Unmap();
        }
        queries.Destroy();
        scene.Destroy();
    }
    else
    {
        GTEST_SKIP() << "Ray queries unavailable on this device";
    }
}
