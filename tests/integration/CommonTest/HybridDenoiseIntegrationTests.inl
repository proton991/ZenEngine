// Pixel temporal, reconstruction and reuse regressions in all four submission modes.
class HybridDenoiseIntegrationTest : public ConeVoxelGIIntegrationTest
{
protected:
    static constexpr uint32_t Width  = 17;
    static constexpr uint32_t Height = 9;
    static constexpr uint32_t Pixels = Width * Height;
    RHISampler*               sampler{nullptr};
    RHITexture*               receiver[6]{};
    HybridGIUniformData       hybrid;
    SceneUniformData          scene;

    void SetUp() override
    {
        ConeVoxelGIIntegrationTest::SetUp();
        if (!HasFatalFailure())
        {
            ShaderProgramManager::GetInstance().StoreProgram(
                ZEN_NEW() ComputeFileSP(device, "HybridTextureCheckSP", "VoxelGI/Calibration/hybrid_texture_check.comp.spv"));
            sampler           = device->CreateSampler({});
            hybrid.sampling   = glm::uvec4(0, 1, 32, 1);
            hybrid.rejection  = Vec4(.01f, 1, 1, 0);
            hybrid.bounce     = glm::uvec4(1, 1, 0, 0);
            hybrid.reflection = glm::uvec4(1, 1, 0, 0);
            scene.viewPos     = Vec4(0, 0, 3, 0);
            scene.lightInfo   = Vec4(0, 0, 0, 1);
        }
    }

    RHITexture* Texture(DataFormat format,
                        uint32_t   width  = Width,
                        uint32_t   height = Height,
                        uint32_t   layers = 1,
                        uint32_t   mips   = 1)
    {
        RHITextureCreateInfo info;
        info.type        = RHITextureType::e2D;
        info.width       = width;
        info.height      = height;
        info.depth       = 1;
        info.format      = format;
        info.arrayLayers = layers;
        info.mipmaps     = mips;
        info.usageFlags.SetFlags(RHITextureUsageFlagBits::eSampled, RHITextureUsageFlagBits::eStorage,
                                 RHITextureUsageFlagBits::eTransferDst);
        RHITexture* texture = device->CreateTexture(info);
        EXPECT_NE(texture, nullptr);
        textures.push_back(texture);
        return texture;
    }

    template <class T> RHITexture* Upload(RenderGraph&         graph,
                                          DataFormat           format,
                                          const HeapVector<T>& values,
                                          uint32_t             width  = Width,
                                          uint32_t             height = Height,
                                          uint32_t             layers = 1)
    {
        RHITexture* texture = Texture(format, width, height, layers);
        RHIBuffer*  upload =
            Buffer(static_cast<uint32_t>(values.size() * sizeof(T)), RHIBufferAllocateType::eCPUWrite, values.data());
        graph.GetResourceManager()->ImportHostWrittenBuffer(upload);
        RHIBufferTextureCopyRegion region;
        region.textureSize = RHIOffset3D(width, height, 1);
        region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
        region.textureSubresources.layerCount = layers;
        graph.AddTransferPass("UploadHybridFixture").CopyBufferToTexture(upload, texture, region);
        return texture;
    }

    void Receivers(RenderGraph& graph)
    {
        HeapVector<float>      depth(Pixels, .5f);
        HeapVector<Vec4>       positions(Pixels, Vec4(0, 0, 0, 1));
        HeapVector<glm::uvec2> surface(Pixels, glm::uvec2(0x80008000u, 1));
        depth[0]                = 1;
        surface[1].y            = 0;
        positions[Pixels - 1].z = 1;
        surface[Pixels - 1].y   = 99;
        receiver[0]             = Upload(graph, DataFormat::eR32SFloat, depth);
        receiver[1] = Upload(graph, DataFormat::eR32G32B32A32SFloat, HeapVector<Vec4>(Pixels, Vec4(.5f, .5f, 0, 0)));
        receiver[2] = Upload(graph, DataFormat::eR32G32B32A32SFloat, HeapVector<Vec4>(Pixels, Vec4(0, .5f, 0, 0)));
        receiver[3] = Upload(graph, DataFormat::eR32G32UInt, surface);
        receiver[4] = Upload(graph, DataFormat::eR32G32SFloat, HeapVector<Vec2>(Pixels, Vec2(0)));
        receiver[5] = Upload(graph, DataFormat::eR32G32B32A32SFloat, positions);
    }

    void Inputs(RDGComputePassDesc& pass, bool lighting = true)
    {
        const char* const names[] = {"receiverDepth",   "receiverNormal", "receiverRoughness",
                                     "receiverSurface", "receiverMotion", "receiverPosition"};
        pass.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
        pass.BindValue("uHybridData", hybrid);
        if (lighting)
        {
            pass.BindValue("uSceneData", scene);
        }
        for (uint32_t index = 0; index < 6; ++index)
        {
            pass.BindSampledTexture(names[index], sampler, receiver[index]->GetDefaultView());
        }
    }

    void Dispatch(RenderGraph& graph, RDGComputePassDesc&& pass, uint32_t width, uint32_t height)
    {
        const glm::uvec2 groups((width + 7) / 8, (height + 7) / 8);
        graph.AddComputePass(std::move(pass)).RecordPassCommands([groups](RDGPassCmdEncoder& encoder) {
            encoder.Dispatch(groups.x, groups.y, 1);
        });
    }

    void RecordPreblur(RenderGraph&      graph,
                       RHITexture* const raw[3],
                       RHITexture*       reflection,
                       RHITexture*       distance,
                       RHITexture* const history[5],
                       RHITexture*       pyramids[2],
                       bool              skyLight = true)
    {
        RDGComputePassDesc preblur;
        preblur.SetShaderProgramName(skyLight ? "GIDenoisePreblurSP" : "GIDenoisePreblurNoSkySP");
        Inputs(preblur);
        const char* const names[] = {"Sky", "Bounce", "Specular"};
        for (uint32_t signal = 0; signal < 3; ++signal)
        {
            preblur.BindSampledTexture(NameID(fmt::format("raw{}", names[signal])), sampler, raw[signal]->GetDefaultView());
        }
        preblur.BindSampledTexture("rawReflection", sampler, reflection->GetDefaultView());
        preblur.BindSampledTexture("rawDiffuseDistance", sampler, distance->GetDefaultView());
        const char* const historyNames[] = {"previousLength", "previousReflection", "previousPosition", "previousNormal",
                                            "previousReceiver"};
        for (uint32_t index = 0; index < 5; ++index)
        {
            preblur.BindSampledTexture(historyNames[index], sampler, history[index]->GetDefaultView());
        }
        constexpr uint32_t paddedWidth  = (Width + 7) / 8 * 8;
        constexpr uint32_t paddedHeight = (Height + 7) / 8 * 8;
        for (uint32_t kind = 0; kind < 2; ++kind)
        {
            const uint32_t   firstLevel = kind;
            const DataFormat format     = kind == 0 ? DataFormat::eR16G16B16A16SFloat : DataFormat::eR32G32B32A32UInt;
            const uint32_t   layers     = kind == 0 ? 3u : 2u;
            pyramids[kind] = Texture(format, paddedWidth >> firstLevel, paddedHeight >> firstLevel, layers, 4 - firstLevel);
            for (uint32_t mip = 0; mip < 4 - firstLevel; ++mip)
            {
                RHITextureViewCreateInfo view;
                view.type         = RHITextureType::e2D;
                view.format       = format;
                view.baseMipLevel = mip;
                view.arrayLayers  = layers;
                preblur.BindStorageImage(NameID(fmt::format("{}Mip{}", kind == 0 ? "signal" : "guide", mip + firstLevel)),
                                         pyramids[kind]->CreateView(view), RDGContentGuarantee::eFullWrite);
            }
        }
        Dispatch(graph, std::move(preblur), Width, Height);
    }

    RHIBuffer* ReadBuffer(RenderGraph& graph, RHIBuffer* source)
    {
        RHIBuffer* readback = Buffer(static_cast<uint32_t>(source->GetRequiredSize()), RHIBufferAllocateType::eCPURead);
        graph.AddTransferPass("ReadHybridFixture").CopyBuffer(source, readback, {0, 0, source->GetRequiredSize()}).NeverCull();
        return readback;
    }

    RHIBuffer* ReadColor(RenderGraph& graph, RHITextureView* source, uint32_t width, uint32_t height)
    {
        RHIBuffer*         output = Buffer(width * height * sizeof(Vec4), RHIBufferAllocateType::eGPU);
        RDGComputePassDesc pass;
        pass.SetShaderProgramName("HybridTextureCheckSP");
        pass.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
        pass.BindSampledTexture("source", sampler, source);
        pass.BindStorageBuffer("Results", output, RDGContentGuarantee::eFullWrite);
        Dispatch(graph, std::move(pass), width, height);
        return ReadBuffer(graph, output);
    }

    RHIBuffer* ReadColor(RenderGraph& graph, RHITexture* source)
    {
        return ReadColor(graph, source->GetDefaultView(), source->GetWidth(), source->GetHeight());
    }

    RHIBuffer* ReadValidation(RenderGraph& graph, RHITexture* source)
    {
        ShaderProgramManager::GetInstance().StoreProgram(ZEN_NEW() ComputeFileSP(
            device, "DenoiserValidationCheckSP", "VoxelGI/Calibration/denoiser_validation_check.comp.spv"));
        RHIBuffer* output = Buffer(Width * Height * source->GetArrayLayers() * sizeof(Vec4), RHIBufferAllocateType::eGPU);
        RDGComputePassDesc pass;
        pass.SetShaderProgramName("DenoiserValidationCheckSP");
        pass.BindSampledTexture("source", sampler, source->GetDefaultView());
        pass.BindStorageBuffer("Results", output, RDGContentGuarantee::eFullWrite);
        Dispatch(graph, std::move(pass), Width, Height);
        return ReadBuffer(graph, output);
    }

    void Execute(RenderGraph& graph)
    {
        ASSERT_TRUE(graph.End()) << graph.GetResult().message;
        ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
        device->FlushRHIThread();
        device->WaitForIdle();
    }

    static void ExpectColor(const Vec4& actual, const Vec4& expected)
    {
        for (uint32_t component = 0; component < 4; ++component)
        {
            EXPECT_NEAR(actual[component], expected[component], .002f);
        }
    }
};

TEST_P(HybridDenoiseIntegrationTest, OutputStabilizationClampsAndRejectsUnrelatedSurfaces)
{
    const char* const previousNames[] = {"previousSky",    "previousSpecular", "previousBounce", "previousPosition",
                                         "previousNormal", "previousSurface",  "previousAge"};
    const char* const outputNames[]   = {"outputSky",    "outputSpecular", "outputBounce", "outputPosition",
                                         "outputNormal", "outputSurface",  "outputAge"};
    const DataFormat  formats[]       = {DataFormat::eR16G16B16A16SFloat,
                                         DataFormat::eR16G16B16A16SFloat,
                                         DataFormat::eR16G16B16A16SFloat,
                                         DataFormat::eR32G32B32A32SFloat,
                                         DataFormat::eR16G16UNORM,
                                         DataFormat::eR32G32UInt,
                                         DataFormat::eR16UInt};
    for (uint32_t iteration = 0; iteration < 24; ++iteration)
    {
        SCOPED_TRACE(iteration);
        const uint32_t scenario = iteration % 12;
        const bool     skyLight = iteration < 12;
        RenderGraph    graph("GIOutputStabilization");
        ASSERT_TRUE(graph.Begin());
        Receivers(graph);
        hybrid.sampling.z            = scenario == 11 ? 1u : 32u;
        hybrid.sampling.w            = scenario == 1 ? 0u : 1u;
        hybrid.reservoir.w           = scenario >= 10 ? 1u : 0u;
        hybrid.resolution            = glm::uvec4(Width, Height, 2, scenario == 8 ? 1u : 0u);
        hybrid.previousViewDirection = Vec4(0, 0, 1, 0);
        HeapVector<Vec4> colors(Pixels);
        for (uint32_t index = 0; index < Pixels; ++index)
        {
            colors[index] = Vec4(index % 2 == 0 ? .2f : .8f);
        }
        RHITexture* input = Upload(graph, DataFormat::eR32G32B32A32SFloat, colors);
        RHITexture* skyInput =
            skyLight ? input : Upload(graph, DataFormat::eR32G32B32A32SFloat, HeapVector<Vec4>{Vec4(0, 0, 0, 1)}, 1, 1);
        const Vec4  oldColor(scenario == 6 ? 10.0f : .5f);
        RHITexture* previous[7]{};
        for (uint32_t index = 0; index < 3; ++index)
        {
            previous[index] = index == 0 && !skyLight
                                ? skyInput
                                : Upload(graph, DataFormat::eR32G32B32A32SFloat, HeapVector<Vec4>(Pixels, oldColor));
        }
        previous[3] = Upload(graph, DataFormat::eR32G32B32A32SFloat,
                             HeapVector<Vec4>(Pixels, Vec4(0, 0, scenario == 3 ? 1.0f : 0.0f, scenario == 5 ? .9f : .5f)));
        previous[4] = Upload(graph, DataFormat::eR32G32B32A32SFloat,
                             HeapVector<Vec4>(Pixels, scenario == 4 ? Vec4(0) : Vec4(.5f, .5f, 0, 0)));
        previous[5] = Upload(graph, DataFormat::eR32G32UInt,
                             HeapVector<glm::uvec2>(Pixels, glm::uvec2(0x80008000u, scenario == 2 ? 2u : 1u)));
        previous[6] = Upload(graph, DataFormat::eR16UInt, HeapVector<uint16_t>(Pixels, scenario >= 10 ? 0x4040u : 0x0808u));
        if (scenario >= 8)
        {
            // The CPU's unchanged-receiver flag covers the complete receiver image, including the isolated last pixel.
            HeapVector<Vec4>       positions(Pixels, Vec4(0, 0, 0, .5f));
            HeapVector<glm::uvec2> surfaces(Pixels, glm::uvec2(0x80008000u, 1));
            positions[Pixels - 1].z = 1;
            surfaces[0].y = surfaces[1].y = 0;
            surfaces[Pixels - 1].y        = 99;
            previous[3]                   = Upload(graph, DataFormat::eR32G32B32A32SFloat, positions);
            previous[5]                   = Upload(graph, DataFormat::eR32G32UInt, surfaces);
        }
        if (scenario == 7)
        {
            receiver[4] = Upload(graph, DataFormat::eR32G32SFloat, HeapVector<Vec2>(Pixels, Vec2(4, 0)));
        }
        RHITexture*        outputs[7]{};
        RDGComputePassDesc pass;
        pass.SetShaderProgramName(skyLight ? "GIDenoiseStabilizeSP" : "GIDenoiseStabilizeNoSkySP");
        Inputs(pass);
        pass.BindSampledTexture("inputSky", sampler, skyInput->GetDefaultView());
        pass.BindSampledTexture("inputSpecular", sampler, input->GetDefaultView());
        pass.BindSampledTexture("inputBounce", sampler, input->GetDefaultView());
        for (uint32_t index = 0; index < 7; ++index)
        {
            outputs[index] = index == 0 && !skyLight ? Texture(formats[index], 1, 1) : Texture(formats[index]);
            pass.BindSampledTexture(previousNames[index], sampler, previous[index]->GetDefaultView());
            pass.BindStorageImage(outputNames[index], outputs[index]->GetDefaultView(), RDGContentGuarantee::eFullWrite);
        }
        Dispatch(graph, std::move(pass), Width, Height);
        RHIBuffer* readbacks[3]{};
        for (uint32_t index = 0; index < 3; ++index)
        {
            readbacks[index] = ReadColor(graph, outputs[index]);
        }
        Execute(graph);
        for (uint32_t index = 0; index < 3; ++index)
        {
            const HeapVector<Vec4> values = ReadStaticBuffer<Vec4>(readbacks[index]);
            if (index == 0 && !skyLight)
            {
                ASSERT_EQ(values.size(), 1u);
                ExpectColor(values[0], Vec4(0));
            }
            else
            {
                ASSERT_EQ(values.size(), Pixels);
                ExpectColor(values[0], Vec4(0));
                ExpectColor(values[1], Vec4(0));
                const bool reused = scenario == 0 || scenario == 6 || scenario >= 8 || (scenario == 5 && index != 1);
                // The new denoiser keeps mature edge history and honors historyFrames * divisor squared.
                const float accumulated = scenario == 10 ? .5f - .3f / 65.0f : scenario == 11 ? .425f : .4625f;
                const float expected    = reused ? (scenario == 6 ? .725f : accumulated) : .2f;
                ExpectColor(values[4 * Width + 4], Vec4(expected, expected, expected, index == 1 ? expected : .2f));
                // A thin receiver retains its own matching history within the current colour bounds.
                // A different previous receiver still cannot supply that history.
                const float isolated = scenario >= 8 ? accumulated : colors[Pixels - 1].r;
                ExpectColor(values[Pixels - 1], Vec4(isolated, isolated, isolated, index == 1 ? isolated : .2f));
            }
        }
    }
}

TEST_P(HybridDenoiseIntegrationTest, DiagnosticTemporalKeepsMissingSamplesOutOfAllHistories)
{
    const char* const names[]    = {"Sky",    "Specular", "Moments", "Length",        "Position",
                                    "Normal", "Receiver", "Bounce",  "BounceMoments", "Reflection"};
    const DataFormat  formats[]  = {DataFormat::eR32G32B32A32SFloat, DataFormat::eR32G32B32A32SFloat,
                                    DataFormat::eR32G32B32A32SFloat, DataFormat::eR16G16B16A16SFloat,
                                    DataFormat::eR32G32B32A32SFloat, DataFormat::eR16G16SFloat,
                                    DataFormat::eR32G32UInt,         DataFormat::eR32G32B32A32SFloat,
                                    DataFormat::eR32G32SFloat,       DataFormat::eR32G32B32A32SFloat};
    const Vec4        previous[] = {Vec4(.3f, .6f, .9f, .75f),
                                    Vec4(.2f, .4f, .8f, .25f),
                                    Vec4(.6f, .36f, .25f, .0625f),
                                    Vec4(7, 5, 0, .5f),
                                    Vec4(0, 0, 0, .5f),
                                    Vec4(.5f, .5f, 0, 0),
                                    Vec4(0),
                                    Vec4(.1f, .2f, .3f, 9),
                                    Vec4(.2f, .04f, 0, 0),
                                    Vec4(.25f, .4f, .16f, 11)};
    for (uint32_t scenario = 0; scenario < 3; ++scenario)
    {
        SCOPED_TRACE(scenario);
        RenderGraph graph("DiagnosticMissingSamples");
        ASSERT_TRUE(graph.Begin());
        Receivers(graph);
        hybrid.rejection.w           = 1;
        hybrid.previousViewDirection = Vec4(0, 0, 1, 0);
        hybrid.previousViewPosition  = scene.viewPos;
        hybrid.sampling.w            = scenario == 2 ? 0u : 1u;
        hybrid.bounce.y = hybrid.reflection.y = hybrid.sampling.w;
        RDGComputePassDesc pass;
        pass.SetShaderProgramName("HybridTemporalReferenceSP");
        Inputs(pass);
        RHITexture* output[10]{};
        for (uint32_t signal = 0; signal < 10; ++signal)
        {
            RHITexture* history =
                signal == 6 ? Upload(graph, DataFormat::eR32G32UInt, HeapVector<glm::uvec2>(Pixels, glm::uvec2(0x80008000u, 1)))
                            : Upload(graph, DataFormat::eR32G32B32A32SFloat, HeapVector<Vec4>(Pixels, previous[signal]));
            output[signal] = Texture(formats[signal]);
            pass.BindSampledTexture(NameID(fmt::format("previous{}", names[signal])), sampler, history->GetDefaultView());
            pass.BindStorageImage(NameID(fmt::format("next{}", names[signal])), output[signal]->GetDefaultView(),
                                  RDGContentGuarantee::eFullWrite);
        }
        for (const char* name : {"rawSky", "rawSpecular", "rawBounce", "rawBouncePrevious", "rawReflectionPrevious"})
        {
            RHITexture* raw = Upload(graph, DataFormat::eR32G32B32A32SFloat,
                                     HeapVector<Vec4>(Pixels, Vec4(0, 0, 0, scenario == 0 ? 0.0f : -1.0f)));
            pass.BindSampledTexture(name, sampler, raw->GetDefaultView());
        }
        RHITexture* guide = Upload(graph, DataFormat::eR32G32B32A32SFloat,
                                   HeapVector<Vec4>(Pixels, Vec4(0, scenario == 0 ? 0.0f : -1.0f, 0, 0)));
        pass.BindSampledTexture("rawReflection", sampler, guide->GetDefaultView());
        Dispatch(graph, std::move(pass), Width, Height);
        RHIBuffer*     readback[5]{};
        const uint32_t signals[] = {0, 1, 3, 7, 9};
        for (uint32_t index = 0; index < 5; ++index)
        {
            readback[index] = ReadColor(graph, output[signals[index]]);
        }
        Execute(graph);
        ASSERT_FALSE(HasFatalFailure());
        for (uint32_t index = 0; index < 5; ++index)
        {
            const uint32_t signal = signals[index];
            SCOPED_TRACE(signal);
            const HeapVector<Vec4> result   = ReadStaticBuffer<Vec4>(readback[index]);
            Vec4                   expected = scenario == 2 ? Vec4(0) : previous[signal];
            if (scenario == 0)
            {
                if (signal == 0)
                {
                    expected *= 7.0f / 8.0f;
                }
                else if (signal == 1)
                {
                    expected = Vec4(Vec3(expected) * (11.0f / 12.0f), expected.w * (5.0f / 6.0f));
                }
                else if (signal == 3)
                {
                    expected = Vec4(8, 6, 0, .5f);
                }
                else if (signal == 7)
                {
                    expected = Vec4(Vec3(expected) * .9f, 10);
                }
                else
                {
                    expected = Vec4(0, expected.y * (11.0f / 12.0f), expected.z * (11.0f / 12.0f), 12);
                }
            }
            if (signal == 3 && scenario == 2)
            {
                // Reset reason 1 for the diffuse history plus 16 times reset reason 1 for reflections.
                expected.z = 17;
                expected.w = .5f;
            }
            if (signal == 9)
            {
                expected.x = 0; // No current hit-distance guide.
            }
            ExpectColor(result[76], expected);
        }
    }
}

TEST_P(HybridDenoiseIntegrationTest, ReconstructionSearchesRingAndScalesPlaneLimitWithViewDistance)
{
    const uint32_t reducedWidth = (Width + 1) / 2, reducedHeight = (Height + 1) / 2;
    const uint32_t reducedTexels = reducedWidth * reducedHeight;
    for (uint32_t scenario = 0; scenario < 8; ++scenario)
    {
        SCOPED_TRACE(scenario);
        const uint32_t geometry = scenario % 4;
        const bool     hardware = scenario < 4;
        RenderGraph    graph("ReconstructionSearch");
        ASSERT_TRUE(graph.Begin());
        Receivers(graph);
        hybrid.resolution              = glm::uvec4(Width, Height, 2, 0);
        hybrid.provider.x              = hardware ? 1u : 0u;
        hybrid.reservoir.w             = 1u;
        hybrid.rejection.x             = .001f;
        hybrid.bounce.x                = 1;
        scene.viewPos                  = Vec4(0, 0, geometry == 2 ? .5f : 3.0f, 1);
        const bool             foreign = geometry == 0 || geometry == 3;
        HeapVector<Vec4>       positions(reducedTexels, Vec4(0, 0, foreign ? 0.0f : .002f, 1));
        HeapVector<glm::uvec2> surfaces(reducedTexels, glm::uvec2(0x80008000u, foreign ? 7u : 1u));
        HeapVector<Vec4>       values(reducedTexels, Vec4(foreign ? 9.0f : .3f, foreign ? 9.0f : .4f, foreign ? 9.0f : .5f, 1));
        // Receiver (8, 4) reads source (3.5, 1.5) from base texel (3, 1): bilinear window x 2..5,
        // 8x8 window x 0..7. Texel x 7 lies in the 8x8 ring, x 8 only in the 16x16 ring.
        const uint32_t ring = 2 * reducedWidth + (geometry == 3 ? 8u : 7u);
        surfaces[ring].y    = 1;
        values[ring]        = Vec4(.3f, .4f, .5f, 1);
        if (geometry == 2)
        {
            values[2 * reducedWidth + 4] = Vec4(.7f, .8f, .9f, 1);
        }
        RDGComputePassDesc pass;
        pass.SetShaderProgramName("HybridReconstructSP");
        Inputs(pass);
        RHITexture* value = Upload(graph, DataFormat::eR32G32B32A32SFloat, values, reducedWidth, reducedHeight);
        for (const char* name : {"inputSky", "inputSpecular", "inputBounce"})
        {
            pass.BindSampledTexture(name, sampler, value->GetDefaultView());
        }
        RHITexture* repaired = Upload(graph, DataFormat::eR32G32B32A32SFloat,
                                      HeapVector<Vec4>(reducedTexels, Vec4(.2f, .3f, .4f, 1)), reducedWidth, reducedHeight);
        for (const char* name : {"fixedSky", "fixedSpecular", "fixedBounce"})
        {
            pass.BindSampledTexture(name, sampler, repaired->GetDefaultView());
        }
        RHITexture* reducedPosition = Upload(graph, DataFormat::eR32G32B32A32SFloat, positions, reducedWidth, reducedHeight);
        RHITexture* reducedNormal  = Upload(graph, DataFormat::eR32G32B32A32SFloat,
                                            HeapVector<Vec4>(reducedTexels, Vec4(.5f, .5f, 0, 0)), reducedWidth, reducedHeight);
        RHITexture* reducedSurface = Upload(graph, DataFormat::eR32G32UInt, surfaces, reducedWidth, reducedHeight);
        // Downsample keeps the representative pixel's own roughness in alpha for material matching.
        RHITexture* reducedRoughness =
            Upload(graph, DataFormat::eR32G32B32A32SFloat, HeapVector<Vec4>(reducedTexels, Vec4(0, .5f, 0, .5f)), reducedWidth,
                   reducedHeight);
        pass.BindSampledTexture("reducedPosition", sampler, reducedPosition->GetDefaultView());
        pass.BindSampledTexture("reducedNormal", sampler, reducedNormal->GetDefaultView());
        pass.BindSampledTexture("reducedSurface", sampler, reducedSurface->GetDefaultView());
        pass.BindSampledTexture("reducedRoughness", sampler, reducedRoughness->GetDefaultView());
        RHITexture* outputs[3] = {Texture(DataFormat::eR16G16B16A16SFloat), Texture(DataFormat::eR16G16B16A16SFloat),
                                  Texture(DataFormat::eR16G16B16A16SFloat)};
        pass.BindStorageImage("outputSky", outputs[0]->GetDefaultView(), RDGContentGuarantee::eFullWrite);
        pass.BindStorageImage("outputSpecular", outputs[1]->GetDefaultView(), RDGContentGuarantee::eFullWrite);
        pass.BindStorageImage("outputBounce", outputs[2]->GetDefaultView(), RDGContentGuarantee::eFullWrite);
        pass.BindStorageImage("missingReceiver", Texture(DataFormat::eR8UInt)->GetDefaultView(),
                              RDGContentGuarantee::eFullWrite);
        Dispatch(graph, std::move(pass), Width, Height);
        RHIBuffer* readback[3] = {ReadColor(graph, outputs[0]), ReadColor(graph, outputs[1]), ReadColor(graph, outputs[2])};
        Execute(graph);
        ASSERT_FALSE(HasFatalFailure());
        // The history-fixed texel is used when no guide matches, independently of tier.
        const Vec4 expected = geometry == 2 ? Vec4(.2f, .3f, .4f, 1) : Vec4(.3f, .4f, .5f, 1);
        for (uint32_t signal = 0; signal < 3; ++signal)
        {
            SCOPED_TRACE(signal);
            const HeapVector<Vec4> actual = ReadStaticBuffer<Vec4>(readback[signal]);
            ExpectColor(actual[4 * Width + 8], expected);
            // Background pixels never acquire the nearest surface's lighting.
            ExpectColor(actual[0], Vec4(0));
        }
    }
}

TEST_P(HybridDenoiseIntegrationTest, DownsampleFootprintAveragesBlockNormalsAndWidensRoughness)
{
    // Block (2, 1) holds two normal-mapped tilts of 14 degrees either side of +Z on one face; its
    // representative pixel (5, 3) has the positive tilt. Block (4, 1) is flat apart from pixel (8, 2),
    // which belongs to another surface with a tilted normal and must not enter the footprint.
    constexpr uint32_t reducedWidth = (Width + 1) / 2, reducedHeight = (Height + 1) / 2;
    constexpr uint32_t tilted = 1 * reducedWidth + 2;
    constexpr uint32_t flat   = 1 * reducedWidth + 4;
    const Vec4         up(.5f, .5f, 0, 0), plus(.6f, .5f, 0, 0), minus(.4f, .5f, 0, 0);
    // Encoded (.6, .5) decodes to normalize(.2, 0, .8): the mean of the pair has length cos(14 degrees).
    const float r        = .8f / std::sqrt(.68f);
    const float variance = 2.0f * (1.0f - r * r) / (3.0f * r - r * r * r);
    const float widened  = std::pow(std::pow(.5f, 4.0f) + variance, .25f);
    for (uint32_t footprint = 0; footprint < 2; ++footprint)
    {
        SCOPED_TRACE(footprint);
        RenderGraph graph("DownsampleFootprint");
        ASSERT_TRUE(graph.Begin());
        Receivers(graph);
        hybrid.resolution = glm::uvec4(Width, Height, 2, 0);
        HeapVector<Vec4>       normals(Pixels, up);
        HeapVector<glm::uvec2> surfaces(Pixels, glm::uvec2(0x80008000u, 1));
        surfaces[1].y          = 0;
        surfaces[Pixels - 1].y = 99;
        normals[2 * Width + 4] = normals[3 * Width + 5] = plus;
        normals[2 * Width + 5] = normals[3 * Width + 4] = minus;
        normals[2 * Width + 8]                          = plus;
        surfaces[2 * Width + 8]                         = glm::uvec2(0x80008000u, 2);
        receiver[1]                                     = Upload(graph, DataFormat::eR32G32B32A32SFloat, normals);
        receiver[3]                                     = Upload(graph, DataFormat::eR32G32UInt, surfaces);
        RDGComputePassDesc pass;
        pass.SetShaderProgramName(footprint != 0 ? "HybridDownsampleFootprintSP" : "HybridDownsampleSP");
        Inputs(pass, false);
        const char* const names[]   = {"reducedDepth",   "reducedNormal", "reducedRoughness",
                                       "reducedSurface", "reducedMotion", "reducedPosition"};
        const DataFormat  formats[] = {DataFormat::eR32SFloat,  DataFormat::eR16G16UNORM,  DataFormat::eR8G8B8A8UNORM,
                                       DataFormat::eR32G32UInt, DataFormat::eR32G32SFloat, DataFormat::eR32G32B32A32SFloat};
        RHITexture*       outputs[6]{};
        for (uint32_t index = 0; index < 6; ++index)
        {
            outputs[index] = Texture(formats[index], reducedWidth, reducedHeight);
            pass.BindStorageImage(names[index], outputs[index]->GetDefaultView(), RDGContentGuarantee::eFullWrite);
        }
        Dispatch(graph, std::move(pass), reducedWidth, reducedHeight);
        RHIBuffer* normal    = ReadColor(graph, outputs[1]);
        RHIBuffer* roughness = ReadColor(graph, outputs[2]);
        Execute(graph);
        ASSERT_FALSE(HasFatalFailure());
        const HeapVector<Vec4> normalValues    = ReadStaticBuffer<Vec4>(normal);
        const HeapVector<Vec4> roughnessValues = ReadStaticBuffer<Vec4>(roughness);
        // The footprint mean of the opposite tilts is +Z with widened roughness; one pixel keeps its tilt.
        const Vec4 tiltedNormal = footprint != 0 ? up : plus;
        EXPECT_NEAR(normalValues[tilted].x, tiltedNormal.x, .001f);
        EXPECT_NEAR(normalValues[tilted].y, tiltedNormal.y, .001f);
        EXPECT_NEAR(roughnessValues[tilted].g, footprint != 0 ? widened : .5f, .005f);
        // The foreign pixel is outside the footprint: the flat block keeps +Z and its roughness.
        EXPECT_NEAR(normalValues[flat].x, .5f, .001f);
        EXPECT_NEAR(roughnessValues[flat].g, .5f, .005f);
        // Alpha keeps the representative's own roughness for reconstruction in both modes.
        EXPECT_NEAR(roughnessValues[tilted].a, .5f, .005f);
        EXPECT_NEAR(roughnessValues[flat].a, .5f, .005f);
    }
    EXPECT_GT(widened, .58f);
}

static HeapVector<uint64_t> PackHalf(const HeapVector<Vec4>& values)
{
    HeapVector<uint64_t> result(values.size());
    for (size_t index = 0; index < values.size(); ++index)
    {
        result[index] = glm::packHalf4x16(values[index]);
    }
    return result;
}

// The shared decisions of forward_material.glsl and the forward bound, evaluated through
// Calibration/hybrid_reuse_check.comp: whether a decal rests on its receiver (inherits GI) or is
// lifted (traces), lobe matching for reflection reuse, the sample bound and the forward layer's
// receiver reference.
TEST_P(HybridDenoiseIntegrationTest, ReuseDecisionsAndBoundReferences)
{
    struct ReuseCase
    {
        Vec4       fragment;
        Vec4       receiver;
        Vec4       receiverNormal;
        Vec4       lobe;
        Vec4       layer;
        Vec4       bound;
        glm::ivec4 pixel;
    };
    static_assert(sizeof(ReuseCase) == 112);
    ShaderProgramManager::GetInstance().StoreProgram(
        ZEN_NEW() ComputeFileSP(device, "HybridReuseCheckSP", "VoxelGI/Calibration/hybrid_reuse_check.comp.spv"));
    const Vec4 z(0, 0, 1, 1);
    const Vec4 rough(0, 0, 1, .6f), layer(0, 0, 1, 1);
    // Decal support: resting, lifted, voxel half-voxel support, flipped plane, view-distance scaling.
    HeapVector<ReuseCase> cases = {
        {Vec4(0, 0, -.5f, 1), Vec4(0, 0, -.5009f, .01f), z, rough, layer, Vec4(100, 1, 0, 0), glm::ivec4(8, 4, 0, 0)},
        {Vec4(0, 0, -.5f, 1), Vec4(0, 0, -.502f, .01f), z, rough, layer, Vec4(5, 1, 0, 0), glm::ivec4(8, 4, 0, 0)},
        {Vec4(0, 0, -.5f, -1), Vec4(0, 0, -.505f, .01f), Vec4(0, 0, 1, 0), rough, layer, Vec4(5, -1, 0, 0),
         glm::ivec4(8, 4, 0, 0)},
        {Vec4(0, 0, -.5f, .3f), Vec4(0, 0, -.5001f, .01f), z, rough, layer, Vec4(0, 0, 0, 0), glm::ivec4(8, 4, 0, 0)},
        {Vec4(0, 0, -3, 1), Vec4(0, 0, -3.002f, .01f), z, rough, layer, Vec4(8, 1, 0, 0), glm::ivec4(8, 4, 0, 0)},
        // Lobes: broad pair, narrow-to-broad, glossy normal change, rough small normal change.
        {Vec4(0, 0, -.5f, 1), Vec4(0, 0, -.5f, .01f), z, Vec4(0, 0, 1, .3f), Vec4(0, 0, 1, .9f), Vec4(0),
         glm::ivec4(8, 4, 0, 0)},
        {Vec4(0, 0, -.5f, 1), Vec4(0, 0, -.5f, .01f), z, Vec4(.2f, 0, .98f, .1f), Vec4(0, 0, 1, .1f), Vec4(0),
         glm::ivec4(8, 4, 0, 0)},
        {Vec4(0, 0, -.5f, 1), Vec4(0, 0, -.5f, .01f), z, Vec4(.3f, 0, .954f, .7f), Vec4(0, 0, 1, .75f), Vec4(0),
         glm::ivec4(8, 4, 0, 0)}};
    const bool     rests[]   = {true, false, true, false, true, true, true, true};
    const bool     lobes[]   = {true, true, true, true, true, false, false, true};
    const float    scales[]  = {.08f, 1, 1, 1, 1, 1, 1, 1};
    const uint32_t caseCount = static_cast<uint32_t>(cases.size());
    RenderGraph    graph("ReuseDecisions");
    ASSERT_TRUE(graph.Begin());
    Receivers(graph);
    hybrid.bounce.x = 1;
    // Interpolated outputs: luminance grows with x.
    HeapVector<Vec4> sky(Pixels), specular(Pixels), bounce(Pixels);
    for (uint32_t index = 0; index < Pixels; ++index)
    {
        const float x   = float(index % Width);
        sky[index]      = Vec4(Vec3(.01f * x), 1);
        bounce[index]   = Vec4(Vec3(.02f * x), 1);
        specular[index] = Vec4(Vec3(.03f * x), 1);
    }
    RHITexture* sampled[3] = {Upload(graph, DataFormat::eR16G16B16A16SFloat, PackHalf(sky)),
                              Upload(graph, DataFormat::eR16G16B16A16SFloat, PackHalf(specular)),
                              Upload(graph, DataFormat::eR16G16B16A16SFloat, PackHalf(bounce))};
    RHIBuffer*  input      = Buffer(caseCount * sizeof(ReuseCase), RHIBufferAllocateType::eCPUWrite, cases.data());
    graph.GetResourceManager()->ImportHostWrittenBuffer(input);
    RHIBuffer*         output = Buffer(caseCount * 2 * sizeof(Vec4), RHIBufferAllocateType::eGPU);
    RDGComputePassDesc pass;
    pass.SetShaderProgramName("HybridReuseCheckSP");
    pass.BindValue("uHybridData", hybrid);
    const char* const names[] = {"receiverDepth",   "receiverNormal", "receiverRoughness",
                                 "receiverSurface", "receiverMotion", "receiverPosition"};
    for (uint32_t index = 0; index < 6; ++index)
    {
        pass.BindSampledTexture(names[index], sampler, receiver[index]->GetDefaultView());
    }
    pass.BindSampledTexture("hybridSky", sampler, sampled[0]->GetDefaultView());
    pass.BindSampledTexture("hybridSpecular", sampler, sampled[1]->GetDefaultView());
    pass.BindSampledTexture("hybridBounce", sampler, sampled[2]->GetDefaultView());
    pass.BindSampledTexture("hybridSurface", sampler, receiver[3]->GetDefaultView());
    pass.BindSampledTexture("hybridDepth", sampler, receiver[0]->GetDefaultView());
    pass.BindStorageBuffer("ReuseCases", input);
    pass.BindStorageBuffer("ReuseResults", output, RDGContentGuarantee::eFullWrite);
    graph.AddComputePass(std::move(pass)).RecordPassCommands([](RDGPassCmdEncoder& encoder) { encoder.Dispatch(1, 1, 1); });
    RHIBuffer* readback = ReadBuffer(graph, output);
    Execute(graph);
    ASSERT_FALSE(HasFatalFailure());
    const HeapVector<Vec4> results = ReadStaticBuffer<Vec4>(readback);
    for (uint32_t index = 0; index < caseCount; ++index)
    {
        SCOPED_TRACE(index);
        EXPECT_EQ(results[index * 2].x, rests[index] ? 1.0f : 0.0f);
        EXPECT_EQ(results[index * 2].y, lobes[index] ? 1.0f : 0.0f);
        EXPECT_NEAR(results[index * 2].z, scales[index], 1e-6f);
    }
    // Forward receiver reference at (8, 4): mean of the 3x3 receivers at x 4, 8, 12 = luminance at x 8.
    // The background (0, 0) is outside this grid.
    ExpectColor(results[1], Vec4(.08f + .16f, .24f, .08f, .24f));
}

TEST_P(HybridDenoiseIntegrationTest, CapturePreservesRawAndReconstructedBounceWithoutVoxelInputs)
{
    RenderGraph& graph = *device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph.Begin());
    Receivers(graph);
    constexpr uint32_t rawWidth  = (Width + 1) / 2;
    constexpr uint32_t rawHeight = (Height + 1) / 2;
    hybrid.resolution            = glm::uvec4(Width, Height, 2, 0);
    HeapVector<Vec4> raw(rawWidth * rawHeight);
    HeapVector<Vec4> reconstructed(Pixels);
    for (uint32_t index = 0; index < raw.size(); ++index)
    {
        raw[index] = Vec4(float(index + 1), 2, 3, 4);
    }
    for (uint32_t index = 0; index < reconstructed.size(); ++index)
    {
        reconstructed[index] = Vec4(float(index + 1000), 5, 6, 7);
    }
    RHITexture*        rawSignal    = Upload(graph, DataFormat::eR32G32B32A32SFloat, raw, rawWidth, rawHeight);
    RHITexture*        outputSignal = Upload(graph, DataFormat::eR32G32B32A32SFloat, reconstructed);
    RHIBuffer*         output = Buffer(Pixels * ZEN_HYBRID_CAPTURE_COMPONENTS * sizeof(Vec4), RHIBufferAllocateType::eGPU);
    RDGComputePassDesc pass;
    pass.SetShaderProgramName("HybridCaptureSP");
    Inputs(pass, false);
    for (const char* name :
         {"rawSky", "rawBounce", "rawSpecular", "rawReflection", "moments", "historyLength", "reflectionHistory"})
    {
        pass.BindSampledTexture(name, sampler, rawSignal->GetDefaultView());
    }
    for (const char* name : {"reconstructedSky", "reconstructedBounce", "reconstructedSpecular"})
    {
        pass.BindSampledTexture(name, sampler, outputSignal->GetDefaultView());
    }
    pass.BindStorageBuffer("HybridCapture", output, RDGContentGuarantee::eFullWrite);
    Dispatch(graph, std::move(pass), Width, Height);
    RHIBuffer* readback = ReadBuffer(graph, output);
    Execute(graph);
    ASSERT_FALSE(HasFatalFailure());
    const HeapVector<Vec4> values = ReadStaticBuffer<Vec4>(readback);
    for (uint32_t y = 0; y < Height; ++y)
    {
        for (uint32_t x = 0; x < Width; ++x)
        {
            const uint32_t pixel     = x + y * Width;
            const uint32_t component = pixel * ZEN_HYBRID_CAPTURE_COMPONENTS;
            const uint32_t rawPixel  = x / 2 + y / 2 * rawWidth;
            ExpectColor(values[component + 0], raw[rawPixel]);
            ExpectColor(values[component + 2], raw[rawPixel]);
            ExpectColor(values[component + 3], reconstructed[pixel]);
            ExpectColor(values[component + 5], reconstructed[pixel]);
        }
    }
}

TEST_P(HybridDenoiseIntegrationTest, RelaxPyramidRepairsMissingSamplesWithoutCrossingSurfaces)
{
    RenderGraph graph("RelaxPyramidRepair");
    ASSERT_TRUE(graph.Begin());
    Receivers(graph);
    hybrid.sampling.w    = 0;
    hybrid.resolution    = glm::uvec4(Width, Height, 1, 0);
    const Vec4  colors[] = {Vec4(.25f, .25f, .25f, 1), Vec4(.5f, .5f, .5f, 1), Vec4(.75f, .75f, .75f, .6f)};
    RHITexture* raw[3]{};
    for (uint32_t signal = 0; signal < 3; ++signal)
    {
        HeapVector<Vec4> values(Pixels, colors[signal]);
        for (uint32_t pixel = 0; pixel < Pixels; ++pixel)
        {
            if ((pixel % Width >= 4 && pixel % Width < 8) || pixel == Pixels - 1)
            {
                values[pixel] = Vec4(0, 0, 0, -1);
            }
        }
        raw[signal] = Upload(graph, DataFormat::eR32G32B32A32SFloat, values);
    }
    HeapVector<Vec2> hit(Pixels, Vec2(0, 1));
    for (uint32_t pixel = 0; pixel < Pixels; ++pixel)
    {
        if ((pixel % Width >= 4 && pixel % Width < 8) || pixel == Pixels - 1)
        {
            hit[pixel].y = -1;
        }
    }
    RHITexture* reflection = Upload(graph, DataFormat::eR32G32SFloat, hit);
    RHITexture* zero       = Upload(graph, DataFormat::eR32G32B32A32SFloat, HeapVector<Vec4>(Pixels, Vec4(0)));
    RHITexture* pyramids[2]{};
    RHITexture* history[] = {zero, zero, receiver[5], receiver[1], receiver[3]};
    RecordPreblur(graph, raw, reflection, zero, history, pyramids);
    constexpr uint32_t paddedWidth  = (Width + 7) / 8 * 8;
    constexpr uint32_t paddedHeight = (Height + 7) / 8 * 8;
    const char* const  names[]      = {"Sky", "Bounce", "Specular"};
    RHIBuffer*         mipReadbacks[3][3]{};
    for (uint32_t signal = 0; signal < 3; ++signal)
    {
        for (uint32_t mip = 1; mip < 4; ++mip)
        {
            RHITextureViewCreateInfo view;
            view.type           = RHITextureType::e2D;
            view.format         = DataFormat::eR16G16B16A16SFloat;
            view.baseMipLevel   = mip;
            view.baseArrayLayer = signal;
            mipReadbacks[signal][mip - 1] =
                ReadColor(graph, pyramids[0]->CreateView(view), paddedWidth >> mip, paddedHeight >> mip);
        }
    }
    RHIBuffer* outputs[3][3]{};
    for (uint32_t scenario = 0; scenario < 3; ++scenario)
    {
        RHITexture* age       = scenario == 0 ? zero
                                              : Upload(graph, DataFormat::eR32G32B32A32SFloat,
                                                       HeapVector<Vec4>(Pixels, Vec4(scenario == 1 ? 6.0f : 8.0f)));
        RHITexture* output[3] = {zero, zero, zero};
        for (uint32_t stage = 0; stage < 4; ++stage)
        {
            RDGComputePassDesc pass;
            pass.SetShaderProgramName(stage == 0 ? NameID("GIDenoiseHistoryFixSP")
                                                 : NameID(fmt::format("GIDenoiseAtrous{}SP", stage - 1)));
            Inputs(pass);
            pass.BindSampledTexture("historyLength", sampler, age->GetDefaultView());
            pass.BindSampledTexture("reflectionHistory", sampler, age->GetDefaultView());
            if (stage == 0)
            {
                pass.BindSampledTexture("preblurSignals", sampler, pyramids[0]->GetDefaultView());
                pass.BindSampledTexture("preblurGuides", sampler, pyramids[1]->GetDefaultView());
                for (const char* name : {"inputFastSky", "inputFastBounce", "inputFastSpecular"})
                {
                    pass.BindSampledTexture(name, sampler, zero->GetDefaultView());
                }
            }
            else
            {
                pass.BindSampledTexture("fastMoments", sampler, zero->GetDefaultView());
            }
            for (uint32_t signal = 0; signal < 3; ++signal)
            {
                pass.BindSampledTexture(NameID(fmt::format("input{}", names[signal])), sampler,
                                        output[signal]->GetDefaultView());
                output[signal] =
                    Texture(stage == 0 && signal != 2 ? DataFormat::eR32G32B32A32SFloat : DataFormat::eR16G16B16A16SFloat);
                pass.BindStorageImage(NameID(fmt::format("output{}", names[signal])), output[signal]->GetDefaultView(),
                                      RDGContentGuarantee::eFullWrite);
            }
            Dispatch(graph, std::move(pass), Width, Height);
        }
        for (uint32_t signal = 0; signal < 3; ++signal)
        {
            outputs[scenario][signal] = ReadColor(graph, output[signal]);
        }
    }
    Execute(graph);
    ASSERT_FALSE(HasFatalFailure());
    for (uint32_t signal = 0; signal < 3; ++signal)
    {
        for (uint32_t mip = 1; mip < 4; ++mip)
        {
            SCOPED_TRACE(mip);
            const HeapVector<Vec4> values = ReadStaticBuffer<Vec4>(mipReadbacks[signal][mip - 1]);
            ASSERT_EQ(values.size(), (paddedWidth >> mip) * (paddedHeight >> mip));
            ExpectColor(values[(4 >> mip) * (paddedWidth >> mip) + (4 >> mip)], colors[signal]);
            // The separate last receiver has no observation; no level invents a black sample for it.
            const Vec4 missing = signal == 2 ? Vec4(-1) : Vec4(0, 0, 0, -1);
            ExpectColor(values[((Height - 1) >> mip) * (paddedWidth >> mip) + ((Width - 1) >> mip)], missing);
        }
        for (uint32_t scenario = 0; scenario < 3; ++scenario)
        {
            SCOPED_TRACE(scenario);
            const HeapVector<Vec4> values = ReadStaticBuffer<Vec4>(outputs[scenario][signal]);
            ASSERT_EQ(values.size(), Pixels);
            ExpectColor(values[0], Vec4(0));
            ExpectColor(values[1], Vec4(0));
            ExpectColor(values[Pixels - 1], Vec4(0));
            const Vec4 value = values[4 * Width + 8];
            if (scenario == 1 && signal != 0)
            {
                // Six-frame bounce/reflection estimates still need current-frame spatial support;
                // keep their own estimate in the blend instead of replacing it.
                for (uint32_t channel = 0; channel < 4; ++channel)
                {
                    EXPECT_GT(value[channel], 0.0f);
                    EXPECT_LT(value[channel], colors[signal][channel]);
                }
            }
            else
            {
                // Missing sky is filled, but valid dark sky must not borrow light from the pyramid.
                // An eight-frame history stays out of repair for every signal.
                ExpectColor(value, scenario == 0 ? colors[signal] : Vec4(0));
            }
        }
    }
}

TEST_P(HybridDenoiseIntegrationTest, SkyRepairPreservesObservedShadowAndUsesIndependentAge)
{
    // The same planar receiver has a dark temporal estimate and a lit coarse neighbourhood.
    // Missing sky may be repaired; one or more valid dark observations must remain dark.
    const float skyAges[] = {0.0f, 1.0f, 3.0f, 6.0f, 8.0f};
    for (uint32_t variant = 0; variant < 4; ++variant)
    {
        const bool validation = (variant & 1u) != 0;
        const bool antiLag    = (variant & 2u) != 0;
        for (float skyAge : skyAges)
        {
            SCOPED_TRACE(variant);
            SCOPED_TRACE(skyAge);
            RenderGraph graph("SkyRepairShadow");
            ASSERT_TRUE(graph.Begin());
            Receivers(graph);
            hybrid.sampling.w = 0;
            hybrid.resolution = glm::uvec4(Width, Height, 1, 0);
            RHITexture* zero  = Upload(graph, DataFormat::eR32G32B32A32SFloat, HeapVector<Vec4>(Pixels, Vec4(0)));
            RHITexture* lit = Upload(graph, DataFormat::eR32G32B32A32SFloat, HeapVector<Vec4>(Pixels, Vec4(.5f, .5f, .5f, 1)));
            RHITexture* raw[]     = {lit, lit, lit};
            RHITexture* history[] = {zero, zero, receiver[5], receiver[1], receiver[3]};
            RHITexture* pyramids[2]{};
            RecordPreblur(graph, raw, zero, zero, history, pyramids);
            // Anti-lag sky age is independent: mature bounce must not suppress missing-sky repair,
            // and young bounce must not force a valid sky into that repair.
            const float bounceAge = antiLag ? (skyAge == 0.0f ? 32.0f : 1.0f) : skyAge;
            RHITexture* age =
                Upload(graph, DataFormat::eR32G32B32A32SFloat, HeapVector<Vec4>(Pixels, Vec4(bounceAge, 32, 0, .5f)));
            RHITexture* reflectionAge =
                Upload(graph, DataFormat::eR32G32B32A32SFloat, HeapVector<Vec4>(Pixels, Vec4(0, 0, 0, 32)));
            RHITexture*        sky = Upload(graph, DataFormat::eR32G32B32A32SFloat,
                                            HeapVector<Vec4>(Pixels, Vec4(0, 0, 0, antiLag ? skyAge : 1.0f)));
            RDGComputePassDesc pass;
            pass.SetShaderProgramName(
                NameID(fmt::format("GIDenoiseHistoryFix{}{}SP", validation ? "Validation" : "", antiLag ? "AntiLag" : "")));
            Inputs(pass);
            pass.BindSampledTexture("preblurSignals", sampler, pyramids[0]->GetDefaultView());
            pass.BindSampledTexture("preblurGuides", sampler, pyramids[1]->GetDefaultView());
            pass.BindSampledTexture("historyLength", sampler, age->GetDefaultView());
            pass.BindSampledTexture("reflectionHistory", sampler, reflectionAge->GetDefaultView());
            RHITexture* diagnostics = nullptr;
            if (validation)
            {
                diagnostics = Texture(DataFormat::eR32G32B32A32SFloat, Width, Height, 2);
                pass.BindStorageImage("repairValidation", diagnostics->GetDefaultView(), RDGContentGuarantee::eFullWrite);
            }
            RHITexture* response = nullptr;
            if (antiLag)
            {
                response = Texture(DataFormat::eR32G32B32A32SFloat);
                pass.BindStorageImage("outputResponseAge", response->GetDefaultView(), RDGContentGuarantee::eFullWrite);
            }
            const char* names[] = {"Sky", "Bounce", "Specular"};
            RHITexture* outputs[3]{};
            for (uint32_t signal = 0; signal < 3; ++signal)
            {
                pass.BindSampledTexture(NameID(fmt::format("input{}", names[signal])), sampler,
                                        (signal == 0 ? sky : zero)->GetDefaultView());
                pass.BindSampledTexture(NameID(fmt::format("inputFast{}", names[signal])), sampler, zero->GetDefaultView());
                outputs[signal] = Texture(signal == 2 ? DataFormat::eR16G16B16A16SFloat : DataFormat::eR32G32B32A32SFloat);
                pass.BindStorageImage(NameID(fmt::format("output{}", names[signal])), outputs[signal]->GetDefaultView(),
                                      RDGContentGuarantee::eFullWrite);
            }
            Dispatch(graph, std::move(pass), Width, Height);
            RHIBuffer* output           = ReadColor(graph, outputs[0]);
            RHIBuffer* diagnosticOutput = validation ? ReadValidation(graph, diagnostics) : nullptr;
            RHIBuffer* responseOutput   = antiLag ? ReadColor(graph, response) : nullptr;
            Execute(graph);
            ASSERT_FALSE(HasFatalFailure());
            const uint32_t         pixel    = 4 * Width + 8;
            const HeapVector<Vec4> values   = ReadStaticBuffer<Vec4>(output);
            const float            expected = skyAge == 0.0f ? .5f : 0.0f;
            EXPECT_NEAR(values[pixel].r, expected, .002f);
            EXPECT_NEAR(values[pixel].g, expected, .002f);
            EXPECT_NEAR(values[pixel].b, expected, .002f);
            ExpectColor(values[0], Vec4(0));
            if (validation)
            {
                const HeapVector<Vec4> diagnosticValues = ReadStaticBuffer<Vec4>(diagnosticOutput);
                EXPECT_FLOAT_EQ(diagnosticValues[Pixels + pixel].x, skyAge == 0.0f ? 3.0f : 0.0f);
            }
            if (antiLag)
            {
                const HeapVector<Vec4> responseValues = ReadStaticBuffer<Vec4>(responseOutput);
                EXPECT_FLOAT_EQ(responseValues[pixel].x, skyAge);
            }
        }
    }
}

TEST_P(HybridDenoiseIntegrationTest, RelaxPreblurReprojectsAndRejectsUnrelatedHistoryAge)
{
    float stableSpecular = 0;
    float youngSpecular  = 0;
    for (uint32_t iteration = 0; iteration < 24; ++iteration)
    {
        SCOPED_TRACE(iteration);
        const uint32_t scenario = iteration % 12;
        const bool     skyLight = iteration < 12;
        RenderGraph    graph("RelaxPreblurAge");
        ASSERT_TRUE(graph.Begin());
        Receivers(graph);
        hybrid.resolution = glm::uvec4(Width, Height, 1, scenario == 0 || scenario == 6 || scenario >= 8 ? 1u : 0u);
        hybrid.sampling.w = scenario == 3 ? 0u : 1u;
        HeapVector<Vec4> diffuse(Pixels, Vec4(0, 0, 0, 1));
        diffuse[3 * Width + 11] = Vec4(4, 4, 4, 1);
        HeapVector<Vec4> specular(Pixels, Vec4(0, 0, 0, 1));
        specular[scenario == 8 || scenario == 9 ? 3 * Width + 10 : 4 * Width + 9] = Vec4(4, 4, 4, 1);
        if (scenario == 9)
        {
            HeapVector<glm::uvec2> surfaces(Pixels, glm::uvec2(0x80008000u, 1));
            surfaces[3 * Width + 10].y = 2;
            receiver[3]                = Upload(graph, DataFormat::eR32G32UInt, surfaces);
        }
        RHITexture* raw[] = {Upload(graph, DataFormat::eR32G32B32A32SFloat, diffuse),
                             Upload(graph, DataFormat::eR32G32B32A32SFloat, diffuse),
                             Upload(graph, DataFormat::eR32G32B32A32SFloat, specular)};
        if (!skyLight || scenario == 7)
        {
            raw[0] = Upload(graph, DataFormat::eR32G32B32A32SFloat,
                            HeapVector<Vec4>(Pixels, Vec4(0, 0, 0, scenario == 7 ? -1.0f : 1.0f)));
        }
        RHITexture*      reflection = Upload(graph, DataFormat::eR32G32SFloat, HeapVector<Vec2>(Pixels, Vec2(100, 1)));
        RHITexture*      distance   = Upload(graph, DataFormat::eR32SFloat, HeapVector<float>(Pixels, 100));
        HeapVector<Vec4> lengths(Pixels, Vec4(128, 128, 0, .5f));
        if (scenario >= 10)
        {
            std::fill(lengths.begin(), lengths.end(), Vec4(1, 1, 0, .5f));
        }
        if (scenario == 2)
        {
            receiver[4] = Upload(graph, DataFormat::eR32G32SFloat, HeapVector<Vec2>(Pixels, Vec2(8.0f / Width, 0)));
            lengths[4 * Width + 12].x = 1;
        }
        RHITexture* history[] = {
            Upload(graph, DataFormat::eR32G32B32A32SFloat, lengths),
            Upload(
                graph, DataFormat::eR32G32B32A32SFloat,
                HeapVector<Vec4>(
                    Pixels, Vec4(0, 0, 0, scenario == 6 || scenario == 8 || scenario == 9 || scenario == 11 ? 1.0f : 128.0f))),
            Upload(graph, DataFormat::eR32G32B32A32SFloat,
                   HeapVector<Vec4>(Pixels, Vec4(0, 0, scenario == 4 ? 1.0f : 0.0f, .5f))),
            Upload(graph, DataFormat::eR32G32B32A32SFloat,
                   HeapVector<Vec4>(Pixels, scenario == 5 ? Vec4(0) : Vec4(.5f, .5f, 0, 0))),
            Upload(graph, DataFormat::eR32G32UInt,
                   HeapVector<glm::uvec2>(Pixels, glm::uvec2(0x80008000u, scenario == 1 ? 2u : 1u)))};
        RHITexture* pyramids[2]{};
        RecordPreblur(graph, raw, reflection, distance, history, pyramids, skyLight);
        RHIBuffer* readbacks[3]{};
        for (uint32_t signal = 0; signal < 3; ++signal)
        {
            RHITextureViewCreateInfo view;
            view.type           = RHITextureType::e2D;
            view.format         = DataFormat::eR16G16B16A16SFloat;
            view.baseArrayLayer = signal == 0 ? 1u : signal == 1 ? 2u : 0u;
            view.arrayLayers    = 1;
            view.mipLevels      = 1;
            readbacks[signal]   = ReadColor(graph, pyramids[0]->CreateView(view), (Width + 7) / 8 * 8, (Height + 7) / 8 * 8);
        }
        Execute(graph);
        ASSERT_FALSE(HasFatalFailure());
        const uint32_t         pixel           = 4 * ((Width + 7) / 8 * 8) + 8;
        const HeapVector<Vec4> blurredDiffuse  = ReadStaticBuffer<Vec4>(readbacks[0]);
        const HeapVector<Vec4> blurredSpecular = ReadStaticBuffer<Vec4>(readbacks[1]);
        if (scenario == 0 || scenario == 6 || scenario == 7 || scenario == 8 || scenario == 9)
        {
            EXPECT_EQ(blurredDiffuse[pixel].r, 0);
        }
        else
        {
            EXPECT_GT(blurredDiffuse[pixel].r, .01f);
        }
        if (scenario == 0)
        {
            stableSpecular = blurredSpecular[pixel].r;
        }
        else if (scenario == 6)
        {
            EXPECT_GT(blurredSpecular[pixel].r, stableSpecular * 2);
            youngSpecular = blurredSpecular[pixel].r;
        }
        else if (scenario == 8)
        {
            // Young specular history needs support beyond the mature diffuse footprint.
            EXPECT_GT(blurredSpecular[pixel].r, .01f);
        }
        else if (scenario == 9)
        {
            EXPECT_EQ(blurredSpecular[pixel].r, 0);
        }
        else if (scenario >= 10)
        {
            // Changing only diffuse history must not change the reflection filter's support.
            EXPECT_FLOAT_EQ(blurredSpecular[pixel].r, scenario == 10 ? stableSpecular : youngSpecular);
        }
        // Traversal availability remains per receiver even when sky radiance is disabled.
        const HeapVector<Vec4> blurredSky = ReadStaticBuffer<Vec4>(readbacks[2]);
        EXPECT_EQ(blurredSky[pixel].a, scenario == 7 ? -1.0f : 1.0f);
    }
}

TEST_P(HybridDenoiseIntegrationTest, RelaxAtrousRetainsCoarseSupportAsHistoryMatures)
{
    float youngValue = 0;
    for (uint32_t scenario = 0; scenario < 3; ++scenario)
    {
        SCOPED_TRACE(scenario);
        RenderGraph graph("RelaxCoarseSupport");
        ASSERT_TRUE(graph.Begin());
        Receivers(graph);
        hybrid.resolution    = glm::uvec4(Width, Height, 1, 0);
        const float      age = scenario == 0 ? 2.0f : 128.0f;
        HeapVector<Vec4> colors(Pixels, Vec4(0));
        colors[4 * Width + 12] = Vec4(1);
        if (scenario == 2)
        {
            HeapVector<glm::uvec2> surfaces(Pixels, glm::uvec2(0x80008000u, 1));
            surfaces[4 * Width + 12].y = 2;
            receiver[3]                = Upload(graph, DataFormat::eR32G32UInt, surfaces);
        }
        RHITexture* input      = Upload(graph, DataFormat::eR32G32B32A32SFloat, colors);
        RHITexture* length     = Upload(graph, DataFormat::eR32G32B32A32SFloat, HeapVector<Vec4>(Pixels, Vec4(age)));
        RHITexture* moments    = Upload(graph, DataFormat::eR32G32B32A32SFloat, HeapVector<Vec4>(Pixels, Vec4(0, 100, 0, 100)));
        RHITexture* reflection = Upload(graph, DataFormat::eR32G32B32A32SFloat, HeapVector<Vec4>(Pixels, Vec4(0, 0, 100, age)));
        RHITexture* outputs[3]{};
        RDGComputePassDesc pass;
        pass.SetShaderProgramName("GIDenoiseAtrous2SP");
        Inputs(pass);
        pass.BindSampledTexture("historyLength", sampler, length->GetDefaultView());
        pass.BindSampledTexture("fastMoments", sampler, moments->GetDefaultView());
        pass.BindSampledTexture("reflectionHistory", sampler, reflection->GetDefaultView());
        const char* names[] = {"Sky", "Bounce", "Specular"};
        for (uint32_t signal = 0; signal < 3; ++signal)
        {
            outputs[signal] = Texture(DataFormat::eR16G16B16A16SFloat);
            pass.BindSampledTexture(NameID(fmt::format("input{}", names[signal])), sampler, input->GetDefaultView());
            pass.BindStorageImage(NameID(fmt::format("output{}", names[signal])), outputs[signal]->GetDefaultView(),
                                  RDGContentGuarantee::eFullWrite);
        }
        Dispatch(graph, std::move(pass), Width, Height);
        RHIBuffer* readback = ReadColor(graph, outputs[0]);
        Execute(graph);
        ASSERT_FALSE(HasFatalFailure());
        const HeapVector<Vec4> values = ReadStaticBuffer<Vec4>(readback);
        ASSERT_EQ(values.size(), Pixels);
        const float filtered = values[4 * Width + 4].r;
        if (scenario == 2)
        {
            EXPECT_EQ(filtered, 0);
        }
        else
        {
            EXPECT_GT(filtered, .001f);
            if (scenario == 0)
            {
                youngValue = filtered;
            }
            else
            {
                EXPECT_LT(filtered, youngValue);
            }
        }
    }
}

TEST_P(HybridDenoiseIntegrationTest, RelaxTemporalRejectsSliversAndKeepsIndependentSignalHistories)
{
    const char* const            names[] = {"Sky",    "Bounce",  "Specular",   "FastSky",  "FastBounce", "FastSpecular",
                                            "Length", "Moments", "Reflection", "Position", "Normal",     "Receiver"};
    HeapVector<HeapVector<Vec4>> baseline(13 * 9);
    for (uint32_t iteration = 0; iteration < 52; ++iteration)
    {
        const uint32_t scenario   = iteration % 13;
        const bool     validation = iteration >= 13;
        const bool     cubic      = iteration >= 26 && iteration < 39;
        const bool     antiLag    = iteration >= 39;
        SCOPED_TRACE(iteration);
        RenderGraph graph("RelaxTemporalSignals");
        ASSERT_TRUE(graph.Begin());
        Receivers(graph);
        scene.viewPos                = Vec4(0, 0, 3, scenario >= 11 ? 1.0f : 0.0f);
        hybrid.sampling              = glm::uvec4(0, 1, 256, scenario == 10 ? 0u : 1u);
        hybrid.resolution            = glm::uvec4(Width * 4, Height * 4, 4, 0);
        hybrid.reflection.x          = scenario == 7 ? 0u : 1u;
        hybrid.previousViewDirection = scenario == 9 ? Vec4(1, 0, 0, 0) : Vec4(0, 0, 1, 0);
        hybrid.previousViewPosition  = scene.viewPos;
        if (scenario == 12)
        {
            hybrid.previousViewPosition.x = .01f;
        }
        if (scenario >= 11)
        {
            // Rotating a perspective camera at a fixed eye creates no reflection parallax.
            // A current miss must keep accepted surface history; translation still needs hit distance.
            receiver[2] = Upload(graph, DataFormat::eR32G32B32A32SFloat, HeapVector<Vec4>(Pixels, Vec4(0, .2f, 0, 0)));
        }
        if (scenario == 1 || scenario == 3 || scenario >= 11)
        {
            receiver[4] = Upload(graph, DataFormat::eR32G32SFloat,
                                 HeapVector<Vec2>(Pixels, Vec2((scenario == 3 ? .995f : .4f) * 2 / Width, 0)));
        }
        const float        oldAge     = scenario == 6 ? 4095.0f : 64.0f;
        const Vec4         previous[] = {Vec4(.4f, .4f, .4f, 1),        Vec4(.2f, .2f, .2f, oldAge),
                                         Vec4(.3f, .3f, .3f, .25f),     Vec4(.4f, .4f, .4f, .16f),
                                         Vec4(.2f, .2f, .2f, .04f),     Vec4(.3f, .3f, .3f, .09f),
                                         Vec4(oldAge, oldAge, 0, .5f),  Vec4(.6f, .36f, .25f, .0625f),
                                         Vec4(.25f, .3f, .09f, oldAge), Vec4(0, 0, scenario == 8 ? 1.0f : 0.0f, .5f),
                                         Vec4(.5f, .5f, 0, 0),          Vec4(0)};
        RDGComputePassDesc pass;
        pass.SetShaderProgramName(antiLag      ? "GIDenoiseTemporalValidationAntiLagSP"
                                  : cubic      ? "GIDenoiseTemporalValidationBicubicSP"
                                  : validation ? "GIDenoiseTemporalValidationSP"
                                               : "GIDenoiseTemporalSP");
        RHITexture* diagnostics = nullptr;
        if (validation)
        {
            diagnostics = Texture(DataFormat::eR32G32B32A32SFloat, Width, Height, 3);
            pass.BindStorageImage("temporalValidation", diagnostics->GetDefaultView(), RDGContentGuarantee::eFullWrite);
        }
        if (antiLag)
        {
            RHITexture* response = Upload(graph, DataFormat::eR32G32B32A32SFloat, HeapVector<Vec4>(Pixels, Vec4(oldAge)));
            pass.BindSampledTexture("previousResponseAge", sampler, response->GetDefaultView());
        }
        Inputs(pass);
        RHITexture* output[12]{};
        for (uint32_t signal = 0; signal < 12; ++signal)
        {
            RHITexture* history = nullptr;
            if (signal == 11)
            {
                HeapVector<glm::uvec2> surfaces(Pixels, glm::uvec2(0x80008000u, scenario == 2 || scenario == 3 ? 2u : 1u));
                if (scenario == 3)
                {
                    surfaces[4 * Width + 4].y = 1;
                }
                history = Upload(graph, DataFormat::eR32G32UInt, surfaces);
            }
            else
            {
                Vec4 value = previous[signal];
                if (scenario >= 11 && (signal == 6 || signal == 9))
                {
                    value.w = .2f;
                }
                if (scenario == 5 && signal < 9)
                {
                    value = signal < 3  ? Vec4(10)
                          : signal == 6 ? previous[6]
                          : signal == 8 ? Vec4(.25f, 0, 0, oldAge)
                                        : Vec4(0);
                }
                history = Upload(graph, DataFormat::eR32G32B32A32SFloat, HeapVector<Vec4>(Pixels, value));
            }
            const DataFormat format = signal == 2  ? DataFormat::eR16G16B16A16SFloat
                                    : signal == 10 ? DataFormat::eR16G16SFloat
                                    : signal == 11 ? DataFormat::eR32G32UInt
                                                   : DataFormat::eR32G32B32A32SFloat;
            output[signal]          = Texture(format);
            pass.BindSampledTexture(NameID(fmt::format("previous{}", names[signal])), sampler, history->GetDefaultView());
            pass.BindStorageImage(NameID(fmt::format("next{}", names[signal])), output[signal]->GetDefaultView(),
                                  RDGContentGuarantee::eFullWrite);
        }
        HeapVector<Vec4> current(Pixels * 3);
        for (uint32_t signal = 0; signal < 3; ++signal)
        {
            const float value = signal == 0 ? .8f : signal == 1 ? .6f : .7f;
            const Vec4  color = scenario == 4 ? Vec4(-1)
                              : scenario == 5 ? Vec4(0)
                                              : Vec4(value, value, value, signal == 2 ? .75f : 1.0f);
            for (uint32_t pixel = 0; pixel < Pixels; ++pixel)
            {
                current[signal * Pixels + pixel] = color;
            }
        }
        RHITexture* signals = Upload(graph, DataFormat::eR32G32B32A32SFloat, current, Width, Height, 3);
        RHITexture* reflection =
            Upload(graph, DataFormat::eR32G32SFloat,
                   HeapVector<Vec2>(Pixels, Vec2(scenario >= 11 ? 0.0f : .25f, scenario == 4 ? -1.0f : 1.0f)));
        pass.BindSampledTexture("preblurSignals", sampler, signals->GetDefaultView());
        pass.BindSampledTexture("rawReflection", sampler, reflection->GetDefaultView());
        Dispatch(graph, std::move(pass), Width, Height);
        RHIBuffer* readback[9]{};
        for (uint32_t signal = 0; signal < 9; ++signal)
        {
            readback[signal] = ReadColor(graph, output[signal]);
        }
        RHIBuffer* diagnosticReadback = validation ? ReadValidation(graph, diagnostics) : nullptr;
        Execute(graph);
        ASSERT_FALSE(HasFatalFailure());
        const bool  rejected = scenario == 2 || scenario == 3 || scenario == 8 || scenario == 10;
        const bool  observed = scenario != 4;
        const float age      = rejected ? 1.0f : oldAge + (observed ? 1.0f : 0.0f);
        if (validation)
        {
            const HeapVector<Vec4> diagnosticValues = ReadStaticBuffer<Vec4>(diagnosticReadback);
            EXPECT_FLOAT_EQ(diagnosticValues[4 * Width + 4].y, age);
            EXPECT_FLOAT_EQ(diagnosticValues[Pixels + 4 * Width + 4].y, std::min(age, 4.0f));
            EXPECT_EQ(uint32_t(diagnosticValues[2 * Pixels + 4 * Width + 4].y) >> 4u, rejected ? 0u : cubic ? 2u : 1u);
        }
        for (uint32_t signal = 0; signal < 9; ++signal)
        {
            const HeapVector<Vec4> values = ReadStaticBuffer<Vec4>(readback[signal]);
            ASSERT_EQ(values.size(), Pixels);
            if (validation && !cubic && !antiLag)
            {
                EXPECT_EQ(std::memcmp(values.data(), baseline[scenario * 9 + signal].data(), Pixels * sizeof(Vec4)), 0);
            }
            else
            {
                baseline[scenario * 9 + signal] = values;
            }
            ExpectColor(values[0], Vec4(0));
            ExpectColor(values[1], Vec4(0));
            const Vec4 value = values[4 * Width + 4];
            if (signal < 3)
            {
                const float oldValue = previous[signal].r;
                const float newValue = signal == 0 ? .8f : signal == 1 ? .6f : .7f;
                const float expected = scenario == 5                  ? 10.0f * (1.0f - 1.0f / age)
                                     : (scenario == 7 && signal == 2) ? 0.0f
                                     : rejected || ((scenario == 9 || scenario == 12) && signal == 2) ? newValue
                                     : observed ? oldValue + (newValue - oldValue) / age
                                                : oldValue;
                for (uint32_t component = 0; component < 3; ++component)
                {
                    // Specular history uses FP16, including bright unclamped temporal intermediates.
                    const float tolerance = signal == 2 ? std::max(.002f, std::abs(expected) / 2048.0f) : .002f;
                    EXPECT_NEAR(value[component], expected, tolerance);
                }
                if (signal == 2)
                {
                    const float visibility = scenario == 5             ? 0.0f
                                           : rejected || scenario == 9 ? .75f
                                           : observed                  ? .25f + .5f / age
                                                                       : .25f;
                    EXPECT_NEAR(value.a, visibility, .002f);
                }
            }
            if (signal == 6)
            {
                EXPECT_FLOAT_EQ(value.x, age);
                EXPECT_FLOAT_EQ(value.y, scenario == 9 ? 1.0f : age);
            }
            if (signal == 8)
            {
                EXPECT_FLOAT_EQ(value.w, scenario == 7 ? 0.0f : scenario == 9 || scenario == 12 ? 1.0f : age);
            }
        }
    }
}

TEST_P(HybridDenoiseIntegrationTest, RelaxTemporalSearchesNeighboursWhenOnlyNormalsReject)
{
    // A rotating view moves reduced-resolution point samples across a normal-mapped surface. Every
    // bilinear tap of texel (4,4) carries a shading normal tilted beyond both thresholds; previous
    // texel (3,3), inside its 3x3 but outside the bilinear footprint, matches. Texel (8,4) has no
    // matching neighbour. A foreign identity among the bilinear taps marks a disocclusion and keeps
    // the reset.
    const char* const  names[]    = {"Sky",    "Bounce",  "Specular",   "FastSky",  "FastBounce", "FastSpecular",
                                     "Length", "Moments", "Reflection", "Position", "Normal",     "Receiver"};
    const char* const  programs[] = {"GIDenoiseTemporalSP", "GIDenoiseTemporalValidationSP",
                                     "GIDenoiseTemporalValidationBicubicSP", "GIDenoiseTemporalValidationAntiLagSP"};
    constexpr float    oldAge     = 64.0f;
    constexpr uint32_t searched   = 4 * Width + 4;
    constexpr uint32_t matching   = 3 * Width + 3;
    constexpr uint32_t isolated   = 4 * Width + 8;
    const Vec4         previous[] = {Vec4(.4f, .4f, .4f, 1),       Vec4(.2f, .2f, .2f, oldAge),   Vec4(.3f, .3f, .3f, .25f),
                                     Vec4(.4f, .4f, .4f, .16f),    Vec4(.2f, .2f, .2f, .04f),     Vec4(.3f, .3f, .3f, .09f),
                                     Vec4(oldAge, oldAge, 0, .5f), Vec4(.6f, .36f, .25f, .0625f), Vec4(.25f, .3f, .09f, oldAge),
                                     Vec4(0, 0, 0, .5f),           Vec4(.5f, .5f, 0, 0),          Vec4(0)};
    for (uint32_t iteration = 0; iteration < 8; ++iteration)
    {
        const uint32_t program = iteration % 4;
        const bool     foreign = iteration >= 4;
        SCOPED_TRACE(iteration);
        RenderGraph graph("RelaxTemporalSearch");
        ASSERT_TRUE(graph.Begin());
        Receivers(graph);
        scene.viewPos                = Vec4(0, 0, 3, 1);
        hybrid.sampling              = glm::uvec4(0, 1, 256, 1);
        hybrid.resolution            = glm::uvec4(Width * 4, Height * 4, 4, 0);
        hybrid.reflection.x          = 1;
        hybrid.previousViewDirection = Vec4(0, 0, 1, 0);
        hybrid.previousViewPosition  = scene.viewPos;
        // A quarter-texel shift: the bilinear taps of (4,4) are (4,4) and (5,4).
        receiver[4] = Upload(graph, DataFormat::eR32G32SFloat, HeapVector<Vec2>(Pixels, Vec2(.25f * 2 / Width, 0)));
        RDGComputePassDesc pass;
        pass.SetShaderProgramName(programs[program]);
        const bool  validation  = program != 0;
        RHITexture* diagnostics = nullptr;
        if (validation)
        {
            diagnostics = Texture(DataFormat::eR32G32B32A32SFloat, Width, Height, 3);
            pass.BindStorageImage("temporalValidation", diagnostics->GetDefaultView(), RDGContentGuarantee::eFullWrite);
        }
        if (program == 3)
        {
            RHITexture* response = Upload(graph, DataFormat::eR32G32B32A32SFloat, HeapVector<Vec4>(Pixels, Vec4(oldAge)));
            pass.BindSampledTexture("previousResponseAge", sampler, response->GetDefaultView());
        }
        Inputs(pass);
        RHITexture* output[12]{};
        for (uint32_t signal = 0; signal < 12; ++signal)
        {
            RHITexture* history = nullptr;
            if (signal == 11)
            {
                HeapVector<glm::uvec2> surfaces(Pixels, glm::uvec2(0x80008000u, 1));
                if (foreign)
                {
                    surfaces[4 * Width + 5].y = 2;
                }
                history = Upload(graph, DataFormat::eR32G32UInt, surfaces);
            }
            else
            {
                HeapVector<Vec4> values(Pixels, previous[signal]);
                if (signal == 10)
                {
                    // Encoded (.7, .5) decodes to a normal 34 degrees from +Z; (.5, .5) is +Z.
                    values           = HeapVector<Vec4>(Pixels, Vec4(.7f, .5f, 0, 0));
                    values[matching] = previous[signal];
                }
                history = Upload(graph, DataFormat::eR32G32B32A32SFloat, values);
            }
            const DataFormat format = signal == 2  ? DataFormat::eR16G16B16A16SFloat
                                    : signal == 10 ? DataFormat::eR16G16SFloat
                                    : signal == 11 ? DataFormat::eR32G32UInt
                                                   : DataFormat::eR32G32B32A32SFloat;
            output[signal]          = Texture(format);
            pass.BindSampledTexture(NameID(fmt::format("previous{}", names[signal])), sampler, history->GetDefaultView());
            pass.BindStorageImage(NameID(fmt::format("next{}", names[signal])), output[signal]->GetDefaultView(),
                                  RDGContentGuarantee::eFullWrite);
        }
        HeapVector<Vec4> current(Pixels * 3);
        for (uint32_t signal = 0; signal < 3; ++signal)
        {
            const float value = signal == 0 ? .8f : signal == 1 ? .6f : .7f;
            for (uint32_t pixel = 0; pixel < Pixels; ++pixel)
            {
                current[signal * Pixels + pixel] = Vec4(value, value, value, signal == 2 ? .75f : 1.0f);
            }
        }
        RHITexture* signals    = Upload(graph, DataFormat::eR32G32B32A32SFloat, current, Width, Height, 3);
        RHITexture* reflection = Upload(graph, DataFormat::eR32G32SFloat, HeapVector<Vec2>(Pixels, Vec2(.25f, 1)));
        pass.BindSampledTexture("preblurSignals", sampler, signals->GetDefaultView());
        pass.BindSampledTexture("rawReflection", sampler, reflection->GetDefaultView());
        Dispatch(graph, std::move(pass), Width, Height);
        RHIBuffer* readback[9]{};
        for (uint32_t signal = 0; signal < 9; ++signal)
        {
            readback[signal] = ReadColor(graph, output[signal]);
        }
        RHIBuffer* diagnosticReadback = validation ? ReadValidation(graph, diagnostics) : nullptr;
        Execute(graph);
        ASSERT_FALSE(HasFatalFailure());
        for (uint32_t pixel : {searched, isolated})
        {
            SCOPED_TRACE(pixel);
            const bool  kept = pixel == searched && !foreign;
            const float age  = kept ? oldAge + 1.0f : 1.0f;
            if (validation)
            {
                const HeapVector<Vec4> diagnosticValues = ReadStaticBuffer<Vec4>(diagnosticReadback);
                EXPECT_FLOAT_EQ(diagnosticValues[pixel].y, age);
                // Footprint nibble: 3 marks history found by the 3x3 search, 0 a reset.
                EXPECT_EQ(uint32_t(diagnosticValues[2 * Pixels + pixel].y) >> 4u, kept ? 3u : 0u);
                EXPECT_EQ(uint32_t(diagnosticValues[2 * Pixels + pixel].z) >> 4u, kept ? 3u : 0u);
            }
            for (uint32_t signal = 0; signal < 9; ++signal)
            {
                const Vec4 value = ReadStaticBuffer<Vec4>(readback[signal])[pixel];
                if (signal < 3)
                {
                    const float oldValue = previous[signal].r;
                    const float newValue = signal == 0 ? .8f : signal == 1 ? .6f : .7f;
                    const float expected = kept ? oldValue + (newValue - oldValue) / age : newValue;
                    for (uint32_t component = 0; component < 3; ++component)
                    {
                        EXPECT_NEAR(value[component], expected, .002f) << signal;
                    }
                    if (signal == 2)
                    {
                        EXPECT_NEAR(value.a, kept ? .25f + .5f / age : .75f, .002f);
                    }
                }
                if (signal == 6)
                {
                    EXPECT_FLOAT_EQ(value.x, age);
                    EXPECT_FLOAT_EQ(value.y, age);
                }
                if (signal == 8)
                {
                    EXPECT_FLOAT_EQ(value.w, age);
                }
            }
        }
    }
}

TEST_P(HybridDenoiseIntegrationTest, RelaxSpatialFastClampPreservesColorVarianceAndRejectsForeignSurfaces)
{
    HeapVector<HeapVector<Vec4>> baseline(15);
    for (uint32_t iteration = 0; iteration < 15; ++iteration)
    {
        const uint32_t scenario   = iteration % 5;
        const bool     validation = iteration >= 5;
        const bool     antiLag    = iteration >= 10;
        SCOPED_TRACE(iteration);
        RenderGraph graph("RelaxSpatialClamp");
        ASSERT_TRUE(graph.Begin());
        Receivers(graph);
        hybrid.resolution = glm::uvec4(Width, Height, 1, 0);
        hybrid.bounce.z = hybrid.reflection.z = scenario == 3 ? 1u : 0u;
        HeapVector<Vec4>       fast(Pixels);
        HeapVector<glm::uvec2> surfaces(Pixels, glm::uvec2(0x80008000u, 1));
        for (uint32_t pixel = 0; pixel < Pixels; ++pixel)
        {
            const float red = (scenario == 0 || scenario >= 3)    ? (pixel % 2 == 0 ? .1f : 1.9f)
                            : scenario == 2 && pixel % Width >= 8 ? 100.0f
                                                                  : 0.0f;
            fast[pixel]     = Vec4(red, 0, 0, 0);
            if (scenario == 2 && pixel % Width >= 8)
            {
                surfaces[pixel].y = 2;
            }
        }
        receiver[3]            = Upload(graph, DataFormat::eR32G32UInt, surfaces);
        RHITexture* responsive = Upload(graph, DataFormat::eR32G32B32A32SFloat, fast);
        RHITexture* slow       = Upload(graph, DataFormat::eR32G32B32A32SFloat,
                                        HeapVector<Vec4>(Pixels, Vec4(scenario >= 3 ? 3.0f : 1.0f, 0, 0, .6f)));
        RHITexture* age        = Upload(graph, DataFormat::eR32G32B32A32SFloat, HeapVector<Vec4>(Pixels, Vec4(64)));
        RHITexture* pyramid    = Upload(graph, DataFormat::eR32G32B32A32SFloat, HeapVector<Vec4>(3, Vec4(0)), 1, 1, 3);
        RHITexture* guides = Upload(graph, DataFormat::eR32G32B32A32UInt, HeapVector<glm::uvec4>(2, glm::uvec4(0)), 1, 1, 2);
        RDGComputePassDesc pass;
        pass.SetShaderProgramName(antiLag      ? "GIDenoiseHistoryFixValidationAntiLagSP"
                                  : validation ? "GIDenoiseHistoryFixValidationSP"
                                               : "GIDenoiseHistoryFixSP");
        RHITexture* diagnostics = nullptr;
        if (validation)
        {
            diagnostics = Texture(DataFormat::eR32G32B32A32SFloat, Width, Height, 2);
            pass.BindStorageImage("repairValidation", diagnostics->GetDefaultView(), RDGContentGuarantee::eFullWrite);
        }
        RHITexture* response = nullptr;
        if (antiLag)
        {
            response = Texture(DataFormat::eR32G32B32A32SFloat);
            pass.BindStorageImage("outputResponseAge", response->GetDefaultView(), RDGContentGuarantee::eFullWrite);
        }
        Inputs(pass);
        pass.BindSampledTexture("preblurSignals", sampler, pyramid->GetDefaultView());
        pass.BindSampledTexture("preblurGuides", sampler, guides->GetDefaultView());
        pass.BindSampledTexture("historyLength", sampler, age->GetDefaultView());
        pass.BindSampledTexture("reflectionHistory", sampler, age->GetDefaultView());
        const char* names[] = {"Sky", "Bounce", "Specular"};
        RHITexture* outputs[3]{};
        for (uint32_t signal = 0; signal < 3; ++signal)
        {
            pass.BindSampledTexture(NameID(fmt::format("input{}", names[signal])), sampler, slow->GetDefaultView());
            pass.BindSampledTexture(NameID(fmt::format("inputFast{}", names[signal])), sampler, responsive->GetDefaultView());
            outputs[signal] = Texture(signal == 2 ? DataFormat::eR16G16B16A16SFloat : DataFormat::eR32G32B32A32SFloat);
            pass.BindStorageImage(NameID(fmt::format("output{}", names[signal])), outputs[signal]->GetDefaultView(),
                                  RDGContentGuarantee::eFullWrite);
        }
        Dispatch(graph, std::move(pass), Width, Height);
        RHIBuffer* readbacks[3] = {ReadColor(graph, outputs[0]), ReadColor(graph, outputs[1]), ReadColor(graph, outputs[2])};
        RHIBuffer* diagnosticReadback = validation ? ReadValidation(graph, diagnostics) : nullptr;
        RHIBuffer* responseReadback   = antiLag ? ReadColor(graph, response) : nullptr;
        Execute(graph);
        ASSERT_FALSE(HasFatalFailure());
        if (antiLag)
        {
            const HeapVector<Vec4> responseValues = ReadStaticBuffer<Vec4>(responseReadback);
            const float            expectedAge    = scenario == 1 || scenario == 2 ? 16.0f : 64.0f;
            EXPECT_FLOAT_EQ(responseValues[4 * Width + 7].y, expectedAge);
            EXPECT_FLOAT_EQ(responseValues[4 * Width + 7].z, expectedAge);
        }
        if (validation)
        {
            const HeapVector<Vec4> diagnosticValues = ReadStaticBuffer<Vec4>(diagnosticReadback);
            const Vec4             movement         = diagnosticValues[4 * Width + 7];
            EXPECT_EQ(movement.x > 0.0f, !antiLag && (scenario == 1 || scenario == 2));
            EXPECT_EQ(movement.y > 0.0f, scenario == 1 || scenario == 2 || scenario == 3);
            EXPECT_EQ(diagnosticValues[Pixels + 4 * Width + 7], Vec4(0));
        }
        for (uint32_t signal = 0; signal < 3; ++signal)
        {
            const HeapVector<Vec4> values = ReadStaticBuffer<Vec4>(readbacks[signal]);
            if (validation && !antiLag)
            {
                EXPECT_EQ(std::memcmp(values.data(), baseline[scenario * 3 + signal].data(), Pixels * sizeof(Vec4)), 0);
            }
            else
            {
                baseline[scenario * 3 + signal] = values;
            }
            ExpectColor(values[0], Vec4(0));
            for (uint32_t x = 7; x <= 8; ++x)
            {
                const Vec4 value = values[4 * Width + x];
                if (scenario == 3 && signal != 0)
                {
                    // A lighting revision must pull stale radiance toward the fast-history range,
                    // including a gradient whose wide stable-history box still admits the old value.
                    EXPECT_GT(value.r, 1.8f);
                    EXPECT_LT(value.r, 2.0f);
                    ExpectColor(Vec4(0, value.g, value.b, value.a), Vec4(0, 0, 0, .6f));
                }
                else
                {
                    // Without the response flag the same mature estimate is retained; sky is independent.
                    // The anti-lag fixture's sky alpha is its own young age (.6), while
                    // bounce/reflection are mature. It must not borrow their clamp eligibility.
                    const float expected = scenario >= 3           ? 3.0f
                                         : antiLag && signal == 0  ? 1.0f
                                         : scenario == 0           ? 1.0f
                                         : scenario == 2 && x == 8 ? 100.0f
                                                                   : 0.0f;
                    ExpectColor(value, Vec4(expected, 0, 0, .6f));
                }
            }
        }
    }
}

TEST_P(HybridDenoiseIntegrationTest, AntiFireflyLimitsYoungOutliersToMatchingNeighbours)
{
    // A young receiver holds one bright, hued sample. Its matching neighbours are grey 1.0 except one
    // at 1.5; a foreign surface at 100 lies inside its 5x5 window. Every raw sample is missing, so the
    // pyramid has no valid taps and young-history repair leaves each receiver's own estimate.
    constexpr uint32_t outlier  = 4 * Width + 8;
    constexpr uint32_t brighter = 3 * Width + 7;
    constexpr uint32_t ordinary = 4 * Width + 6;
    constexpr uint32_t foreign  = 4 * Width + 12;
    constexpr uint32_t isolated = Pixels - 1;
    const Vec4         hot      = Vec4(50, 25, 10, 0);
    const float        hotLuma  = .25f * hot.r + .5f * hot.g + .25f * hot.b;
    const Vec4         limited  = hot * (1.5f / hotLuma);
    const float        ages[]   = {2.0f, 64.0f};
    for (uint32_t variant = 0; variant < 8; ++variant)
    {
        const bool validation  = (variant & 1u) != 0;
        const bool antiLag     = (variant & 2u) != 0;
        const bool antiFirefly = (variant & 4u) != 0;
        for (float age : ages)
        {
            SCOPED_TRACE(variant);
            SCOPED_TRACE(age);
            RenderGraph graph("AntiFirefly");
            ASSERT_TRUE(graph.Begin());
            Receivers(graph);
            hybrid.sampling.w = 0;
            hybrid.resolution = glm::uvec4(Width, Height, 1, 0);
            HeapVector<glm::uvec2> surfaces(Pixels, glm::uvec2(0x80008000u, 1));
            surfaces[1].y        = 0;
            surfaces[isolated].y = 99;
            HeapVector<Vec4> colors(Pixels, Vec4(1, 1, 1, 0));
            for (uint32_t pixel = 0; pixel < Pixels; ++pixel)
            {
                if (pixel % Width >= 10 && pixel != isolated)
                {
                    surfaces[pixel].y = 2;
                    colors[pixel]     = Vec4(100, 100, 100, 0);
                }
            }
            colors[brighter] = Vec4(1.5f, 1.5f, 1.5f, 0);
            colors[outlier]  = hot;
            colors[isolated] = Vec4(50, 50, 50, 0);
            receiver[3]      = Upload(graph, DataFormat::eR32G32UInt, surfaces);
            HeapVector<Vec4> slow(Pixels), sky(Pixels), fast(Pixels);
            for (uint32_t pixel = 0; pixel < Pixels; ++pixel)
            {
                const Vec4  color = colors[pixel];
                const float luma  = .25f * color.r + .5f * color.g + .25f * color.b;
                slow[pixel]       = Vec4(Vec3(color), .6f);
                // Anti-lag reads sky's own age from its alpha.
                sky[pixel]  = Vec4(Vec3(color), antiLag ? age : 1.0f);
                fast[pixel] = Vec4(Vec3(color), luma * luma);
            }
            RHITexture* slowTexture = Upload(graph, DataFormat::eR32G32B32A32SFloat, slow);
            RHITexture* skyTexture  = Upload(graph, DataFormat::eR32G32B32A32SFloat, sky);
            RHITexture* fastTexture = Upload(graph, DataFormat::eR32G32B32A32SFloat, fast);
            RHITexture* ageTexture  = Upload(graph, DataFormat::eR32G32B32A32SFloat, HeapVector<Vec4>(Pixels, Vec4(age)));
            RHITexture* missing   = Upload(graph, DataFormat::eR32G32B32A32SFloat, HeapVector<Vec4>(Pixels, Vec4(0, 0, 0, -1)));
            RHITexture* noHit     = Upload(graph, DataFormat::eR32G32SFloat, HeapVector<Vec2>(Pixels, Vec2(0, -1)));
            RHITexture* zero      = Upload(graph, DataFormat::eR32G32B32A32SFloat, HeapVector<Vec4>(Pixels, Vec4(0)));
            RHITexture* raw[]     = {missing, missing, missing};
            RHITexture* history[] = {zero, zero, receiver[5], receiver[1], receiver[3]};
            RHITexture* pyramids[2]{};
            RecordPreblur(graph, raw, noHit, zero, history, pyramids);
            RDGComputePassDesc pass;
            pass.SetShaderProgramName(NameID(fmt::format("GIDenoiseHistoryFix{}{}{}SP", validation ? "Validation" : "",
                                                         antiLag ? "AntiLag" : "", antiFirefly ? "AntiFirefly" : "")));
            Inputs(pass);
            pass.BindSampledTexture("preblurSignals", sampler, pyramids[0]->GetDefaultView());
            pass.BindSampledTexture("preblurGuides", sampler, pyramids[1]->GetDefaultView());
            pass.BindSampledTexture("historyLength", sampler, ageTexture->GetDefaultView());
            pass.BindSampledTexture("reflectionHistory", sampler, ageTexture->GetDefaultView());
            if (validation)
            {
                RHITexture* diagnostics = Texture(DataFormat::eR32G32B32A32SFloat, Width, Height, 2);
                pass.BindStorageImage("repairValidation", diagnostics->GetDefaultView(), RDGContentGuarantee::eFullWrite);
            }
            if (antiLag)
            {
                RHITexture* response = Texture(DataFormat::eR32G32B32A32SFloat);
                pass.BindStorageImage("outputResponseAge", response->GetDefaultView(), RDGContentGuarantee::eFullWrite);
            }
            const char* names[] = {"Sky", "Bounce", "Specular"};
            RHITexture* outputs[3]{};
            for (uint32_t signal = 0; signal < 3; ++signal)
            {
                pass.BindSampledTexture(NameID(fmt::format("input{}", names[signal])), sampler,
                                        (signal == 0 ? skyTexture : slowTexture)->GetDefaultView());
                pass.BindSampledTexture(NameID(fmt::format("inputFast{}", names[signal])), sampler,
                                        fastTexture->GetDefaultView());
                outputs[signal] = Texture(signal == 2 ? DataFormat::eR16G16B16A16SFloat : DataFormat::eR32G32B32A32SFloat);
                pass.BindStorageImage(NameID(fmt::format("output{}", names[signal])), outputs[signal]->GetDefaultView(),
                                      RDGContentGuarantee::eFullWrite);
            }
            Dispatch(graph, std::move(pass), Width, Height);
            RHIBuffer* readbacks[3] = {ReadColor(graph, outputs[0]), ReadColor(graph, outputs[1]),
                                       ReadColor(graph, outputs[2])};
            Execute(graph);
            ASSERT_FALSE(HasFatalFailure());
            const bool limit = antiFirefly && age < 4.0f;
            for (uint32_t signal = 0; signal < 3; ++signal)
            {
                SCOPED_TRACE(signal);
                const HeapVector<Vec4> values = ReadStaticBuffer<Vec4>(readbacks[signal]);
                // Young outliers fall to the brightest matching neighbour with their hue; the foreign
                // surface does not widen the bound. Mature texels keep the slow-history clamp.
                const Vec4 expected[][2] = {{values[outlier], limit ? limited : hot},
                                            {values[brighter], Vec4(1.5f, 1.5f, 1.5f, 0)},
                                            {values[ordinary], Vec4(1, 1, 1, 0)},
                                            {values[foreign], Vec4(100, 100, 100, 0)},
                                            {values[isolated], Vec4(50, 50, 50, 0)}};
                for (const Vec4(&pair)[2] : expected)
                {
                    // Sky alpha carries its age; bounce and reflection alphas must be unchanged.
                    ExpectColor(Vec4(Vec3(pair[0]), signal == 0 ? 0.0f : pair[0].a - .6f), pair[1]);
                }
            }
        }
    }
}

INSTANTIATE_TEST_SUITE_P(SubmissionModes, HybridDenoiseIntegrationTest, testing::Values(0u, 1u, 2u, 3u));
