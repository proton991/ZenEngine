TEST_P(RayQueryIntegrationTest, TransparentLayersKeepTheirShadowsAndOpaqueReceiverGuides)
{
    if (device->GetGPUInfo().rayQuery.IsUsable())
    {
        ShaderProgramManager::GetInstance().BuildShaderPrograms(device);
        device->InitializeRendererServer();
        RendererServer&        server        = *device->GetRendererServer();
        VoxelGIRuntimeSettings settings      = server.GetVoxelGISettings();
        settings.resolution                  = 64;
        settings.cone.rayProvider            = VoxelGISettings::RayProvider::Hardware;
        settings.cone.pixelResolutionDivisor = 2;
        settings.cone.indirectDiffuse        = false;
        settings.cone.specularOcclusion      = false;
        settings.cone.reflections            = VoxelGISettings::Reflections::Off;
        ASSERT_TRUE(server.ApplyVoxelGISettings(settings));
        server.SetRenderOption(RenderOption::eVoxelGI);
        ForwardBlendSceneFixture fixture(*device);
        ASSERT_TRUE(fixture.Init(1, false, 2, true));
        ASSERT_TRUE(fixture.scene->SetVoxelBounds(sg::AABB(Vec3(-2, -2, -5), Vec3(2, 2, 0))));
        sg::MaterialData material     = fixture.scene->GetMaterialsData()[0];
        material.materialProperties.y = 0;
        material.metallicFactor       = 0;
        ASSERT_TRUE(fixture.scene->UpdateMaterial(0, material));
        SceneLight light;
        light.type         = SceneLightType::eDirectional;
        light.direction    = Vec3(0, 0, -1);
        light.intensity    = 1;
        light.castsShadows = true;
        ASSERT_NE(fixture.scene->GetLights().Add(light), 0u);

        constexpr uint32_t side   = 96;
        constexpr uint32_t pixels = side * side;
        rc::TextureFormat  format;
        format.width = format.height = side;
        format.depth                 = 1;
        format.format                = DataFormat::eR8G8B8A8UNORM;
        RHITexture* color            = device->CreateTextureColorRT(format, {.copyUsage = true}, "RayForwardBlendColor");
        format.format                = DataFormat::eD32SFloat;
        RHITexture* depth            = device->CreateTextureDepthStencilRT(format, {}, "RayForwardBlendDepth");
        textures.push_back(color);
        textures.push_back(depth);
        ASSERT_NE(color, nullptr);
        ASSERT_NE(depth, nullptr);
        RHIFlags<RHIBufferUsageFlagBits> outputUsage;
        outputUsage.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer);
        const RHIFlags<RHIBufferUsageFlagBits> readUsage(RHIBufferUsageFlagBits::eTransferDstBuffer);
        RHIBuffer*         packed   = Buffer(pixels * sizeof(uint32_t), outputUsage, nullptr, RHIBufferAllocateType::eGPU);
        RHIBuffer*         readback = Buffer(pixels * sizeof(uint32_t), readUsage, nullptr, RHIBufferAllocateType::eCPURead);
        constexpr uint32_t captureBytes  = pixels * ZEN_HYBRID_CAPTURE_BYTES_PER_PIXEL;
        RHIBuffer*         capture       = Buffer(captureBytes, outputUsage, nullptr, RHIBufferAllocateType::eGPU);
        RHIBuffer*         captured      = Buffer(captureBytes, readUsage, nullptr, RHIBufferAllocateType::eCPURead);
        constexpr uint32_t lightingBytes = pixels * ZEN_LIGHTING_CAPTURE_BYTES_PER_PIXEL;
        RHIBuffer*         lighting      = Buffer(lightingBytes, outputUsage, nullptr, RHIBufferAllocateType::eGPU);
        RHIBuffer*         litReadback   = Buffer(lightingBytes, readUsage, nullptr, RHIBufferAllocateType::eCPURead);
        ASSERT_NE(packed, nullptr);
        ASSERT_NE(readback, nullptr);
        ASSERT_NE(capture, nullptr);
        ASSERT_NE(captured, nullptr);
        ASSERT_NE(lighting, nullptr);
        ASSERT_NE(litReadback, nullptr);
        DeferredLightingRenderer& renderer = *server.RequestDeferredLightingRenderer();
        renderer.SetHybridCapture(capture, captured);
        renderer.SetLightingCapture(lighting, litReadback);
        const RenderView     view{color, depth, side, side};
        HeapVector<uint32_t> referenceColor;
        HeapVector<uint32_t> transmissionColor;
        HeapVector<Vec4>     referenceGuides;
        LightId              paddingLight = 0;
        for (uint32_t scenario = 0; scenario < 12; ++scenario)
        {
            SCOPED_TRACE(scenario);
            settings.cone.rayTracingPipeline = false;
            ASSERT_TRUE(server.ApplyVoxelGISettings(settings));
            if (scenario == 8)
            {
                // Exercise the high scene-light bit and the separate camera-light value together.
                for (uint32_t index = 1; index < MaxSceneLights; ++index)
                {
                    SceneLight extra;
                    extra.position      = Vec3(0, 0, -4.25f);
                    extra.color         = Vec3(0, 1, 0);
                    extra.intensity     = index == MaxSceneLights - 1 ? .1f : 0;
                    const LightId added = fixture.scene->GetLights().Add(extra);
                    ASSERT_NE(added, 0u);
                    if (index == 1)
                    {
                        paddingLight = added;
                    }
                }
            }
            if (scenario == 10)
            {
                // The camera light must remain independent when the final scene-light bit becomes free.
                ASSERT_TRUE(fixture.scene->GetLights().Remove(paddingLight));
            }
            if (scenario >= 8)
            {
                CameraLightSettings cameraLight;
                cameraLight.enabled      = scenario % 2 == 0;
                cameraLight.followCamera = false;
                cameraLight.position     = Vec3(0, 0, -4.25f);
                cameraLight.color        = Vec3(1, 0, 0);
                cameraLight.intensity    = .1f;
                cameraLight.range        = 10;
                ASSERT_TRUE(fixture.scene->SetCameraLight(cameraLight));
            }
            renderer.SetGPUCullingEnabled(scenario != 0 && scenario != 7);
            ASSERT_TRUE(fixture.scene->SetInstanceTransform(1, glm::translate(Mat4(1), Vec3(scenario == 2 ? .5f : 0, 0, 0))));
            ASSERT_TRUE(fixture.scene->SetInstanceEnabled(0, scenario != 3));
            ASSERT_TRUE(fixture.scene->SetInstanceEnabled(1, scenario != 3));
            if (scenario >= 4)
            {
                // A luminous opaque backdrop isolates screen refraction and absorption from GI.
                sg::MaterialData backdrop = fixture.scene->GetMaterialsData()[1];
                backdrop.emissiveFactor   = Vec4(.5f, .5f, .5f, 1);
                ASSERT_TRUE(fixture.scene->UpdateMaterial(1, backdrop));
                material.materialProperties.w       = 1;
                material.sheenColorTransmission.w   = 1;
                material.clearcoatSheenSpecular.w   = 0;
                material.roughnessFactor            = .05f;
                material.volumeIridescence.x        = scenario == 6 ? 2.0f : .5f;
                material.volumeIridescence.y        = 1;
                material.attenuationColorDispersion = scenario == 5 ? Vec4(.1f, .1f, .1f, 0) : Vec4(1, 1, 1, 0);
                ASSERT_TRUE(fixture.scene->UpdateMaterial(0, material));
            }
            for (uint32_t frame = 0; frame < 4; ++frame)
            {
                ASSERT_EQ(server.DispatchRenderWorkloads(view), RenderFrameResult::eSubmitted);
                device->WaitForPreviousFrames();
                device->NextFrame();
            }
            ASSERT_EQ(server.GetRenderOption(), RenderOption::eVoxelGI);
            ASSERT_NE(renderer.GetHybridGI(), nullptr);
            ASSERT_TRUE(renderer.GetHybridGI()->IsHistoryValid());
            ASSERT_TRUE(renderer.GetHybridGI()->WasCaptureRecorded());
            ASSERT_TRUE(renderer.WasLightingCaptureRecorded());
            ASSERT_TRUE(server.RequestVoxelGI()->UsesHardwareShadows());
            ReadForwardBlendPixels(*device, color, packed, readback);
            ASSERT_FALSE(HasFatalFailure());
            HeapVector<uint32_t> colors(pixels);
            std::memcpy(colors.data(), readback->Map(), pixels * sizeof(uint32_t));
            readback->Unmap();
            HeapVector<Vec4> guides(pixels * ZEN_HYBRID_CAPTURE_COMPONENTS);
            std::memcpy(guides.data(), captured->Map(), captureBytes);
            captured->Unmap();
            const uint32_t center = side / 2 * side + side / 2;
            Vec4           direct;
            std::memcpy(&direct, litReadback->Map() + (center * ZEN_LIGHTING_CAPTURE_COMPONENTS + 7) * sizeof(Vec4),
                        sizeof(Vec4));
            litReadback->Unmap();
            if (SupportsHybridRayTracingPipeline(device->GetGPUInfo()))
            {
                settings.cone.rayTracingPipeline = true;
                ASSERT_TRUE(server.ApplyVoxelGISettings(settings));
                ASSERT_EQ(server.DispatchRenderWorkloads(view), RenderFrameResult::eSubmitted);
                device->WaitForPreviousFrames();
                device->NextFrame();
                ASSERT_TRUE(server.RequestVoxelGI()->UsesRayTracingPipeline());
                ReadForwardBlendPixels(*device, color, packed, readback);
                ASSERT_FALSE(HasFatalFailure());
                const uint32_t* pipelinePixels = reinterpret_cast<const uint32_t*>(readback->Map());
                const std::pair<const uint32_t*, const uint32_t*> mismatch =
                    std::mismatch(static_cast<const uint32_t*>(colors.data()),
                                  static_cast<const uint32_t*>(colors.data()) + pixels, pipelinePixels);
                EXPECT_EQ(mismatch.first, colors.data() + pixels)
                    << "pixel " << mismatch.first - colors.data() << " query " << std::hex << *mismatch.first << " pipeline "
                    << *mismatch.second;
                readback->Unmap();
                Vec4 pipelineDirect;
                std::memcpy(&pipelineDirect, litReadback->Map() + (center * ZEN_LIGHTING_CAPTURE_COMPONENTS + 7) * sizeof(Vec4),
                            sizeof(Vec4));
                litReadback->Unmap();
                EXPECT_EQ(pipelineDirect, direct);
                const Vec4* pipelineGuides = reinterpret_cast<const Vec4*>(captured->Map());
                for (uint32_t pixel = 0; pixel < pixels; ++pixel)
                {
                    for (uint32_t component = 8; component <= 10; ++component)
                    {
                        const uint32_t index = pixel * ZEN_HYBRID_CAPTURE_COMPONENTS + component;
                        EXPECT_EQ(pipelineGuides[index], guides[index]) << pixel << ':' << component;
                    }
                }
                captured->Unmap();
            }
            EXPECT_EQ(direct.w, 1);
            if (scenario == 3 || (scenario >= 8 && scenario % 2 == 0))
            {
                EXPECT_GT(direct.r, .01f);
            }
            else
            {
                EXPECT_NEAR(direct.r, 0, 1e-5f);
            }
            if (scenario >= 8)
            {
                EXPECT_GT(direct.g, .01f);
            }
            EXPECT_NEAR(guides[center * ZEN_HYBRID_CAPTURE_COMPONENTS + 8].z, -4.5f, 1e-4f);
            EXPECT_EQ(guides[center * ZEN_HYBRID_CAPTURE_COMPONENTS + 8].w, 1);
            if (scenario == 0)
            {
                // The front yellow layer sees the light even though the opaque receiver behind it is shadowed.
                EXPECT_GT(colors[center] & 255u, 16u);
                EXPECT_GT((colors[center] >> 8u) & 255u, 16u);
                referenceColor  = colors;
                referenceGuides = guides;
            }
            else
            {
                const bool equal = std::equal(colors.begin(), colors.end(), referenceColor.begin(), referenceColor.end());
                EXPECT_EQ(equal, scenario == 1);
                for (uint32_t pixel = 0; pixel < pixels; ++pixel)
                {
                    for (uint32_t component = 8; component <= 10; ++component)
                    {
                        const uint32_t index = pixel * ZEN_HYBRID_CAPTURE_COMPONENTS + component;
                        EXPECT_EQ(guides[index], referenceGuides[index]) << pixel << ':' << component;
                    }
                }
            }
            if (scenario == 4)
            {
                transmissionColor = colors;
            }
            else if (scenario == 5)
            {
                // Absorption must darken transmitted green through the front yellow layer.
                EXPECT_LT((colors[center] >> 8u) & 255u, (transmissionColor[center] >> 8u) & 255u);
            }
            else if (scenario == 6 || scenario == 7)
            {
                const bool equal = std::equal(colors.begin(), colors.end(), transmissionColor.begin(), transmissionColor.end());
                // Thickness moves refracted samples; switching CPU/GPU drawing preserves them.
                EXPECT_EQ(equal, scenario == 7);
            }
            device->NextFrame();
        }
        renderer.SetHybridCapture(nullptr, nullptr);
        renderer.SetLightingCapture(nullptr, nullptr);
    }
}
