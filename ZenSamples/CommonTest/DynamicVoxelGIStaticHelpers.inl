// Members of the native Vulkan fixture; every allocation is retired in TearDown.
static constexpr uint32_t StaticSide  = 64;
static constexpr uint32_t StaticCells = StaticSide * StaticSide * StaticSide;
struct StaticFixture
{
    StaticVoxelGIInputs inputs;
    VoxelTextures staticVoxels, dynamicVoxels;
    HeapVector<uint32_t> ids;
};
struct StaticReadback
{
    glm::uvec4 status{};
    HeapVector<Vec4> faces;
    HeapVector<uint32_t> masks;
    HeapVector<GIHit> hits;
    HeapVector<glm::uvec2> compactHits;
};
struct IrradianceProbe
{
    Vec4 position;
    Vec4 normal;
};

static uint32_t StaticID(glm::uvec3 p, uint32_t side = StaticSide)
{
    return p.z + side * (p.y + side * p.x);
}

StaticFixture StaticInputs(RenderGraph& graph,
                           const HeapVector<OccupiedCell>& cells,
                           uint32_t side = StaticSide)
{
    const uint32_t cellCount = side * side * side;
    StaticFixture fixture;
    fixture.staticVoxels  = Grid(graph, cells, GI_STATIC, side);
    fixture.dynamicVoxels = Grid(graph, cells, GI_DYNAMIC, side);
    HeapVector<uint32_t> map(cellCount, GI_INVALID_CELL);
    HeapVector<uint32_t> list(cellCount, GI_INVALID_CELL);
    uint32_t dynamicCount = 0;
    for (const OccupiedCell& cell : cells)
    {
        if (cell.objectClass == GI_STATIC)
        {
            const uint32_t id        = StaticID(cell.cell, side);
            map[id]                  = static_cast<uint32_t>(fixture.ids.size());
            list[fixture.ids.size()] = id;
            fixture.ids.push_back(id);
        }
        else
        {
            ++dynamicCount;
        }
    }
    const uint32_t count = static_cast<uint32_t>(fixture.ids.size());
    fixture.inputs = {{Vec4(0, 0, 0, 1), glm::uvec4(side, GI_ALL, 0, 0), glm::uvec4(1, 0, 0, 0)},
                      1,
                      Buffer(cellCount * 4, RHIBufferAllocateType::eCPUWrite, list.data()),
                      Buffer(cellCount * 4, RHIBufferAllocateType::eCPUWrite, map.data()),
                      Buffer(4, RHIBufferAllocateType::eCPUWrite, &count),
                      Buffer(4, RHIBufferAllocateType::eCPUWrite, &dynamicCount),
                      fixture.staticVoxels.pOwner};
    fixture.inputs.normal        = fixture.staticVoxels.pNormal;
    fixture.inputs.dynamicNormal = fixture.dynamicVoxels.pNormal;
    for (RHIBuffer* buffer : {fixture.inputs.occupiedList, fixture.inputs.gridToList,
                              fixture.inputs.occupiedCount, fixture.inputs.dynamicCount})
    {
        graph.GetResourceManager()->ImportHostWrittenBuffer(buffer);
    }
    return fixture;
}

DynamicVoxelGIRenderer* StaticRenderer(uint32_t capacity,
                                       uint32_t radius = 2,
                                       uint32_t side   = StaticSide,
                                       bool compact    = false,
                                       uint32_t rays   = GI_FACE_RAYS)
{
    const uint32_t cellCount = side * side * side;
    uint64_t base = 0;
    EXPECT_EQ(ValidateVoxelClassResources(side, true, UINT64_MAX, 0, device->GetGPUInfo(), base),
              GIResourceStatus::eSuccess);
    DynamicVoxelGISettings settings;
    settings.resolution          = side;
    settings.compactCache        = compact;
    settings.raysPerFace         = rays;
    settings.temporal          = GITemporalMode::eOff;
    settings.spatialFilter     = false;
    settings.environmentLighting = false;
    settings.emissiveLighting    = false;
    settings.neighborRadius    = radius;
    settings.memoryBudgetBytes   = base + cellCount * uint64_t(GI_FRAME_CELL_BYTES) +
        GI_FRAME_FIXED_BYTES + 32 +
        uint64_t(capacity) * GI_FACE_COUNT * rays *
            (compact ? GI_COMPACT_HIT_BYTES : sizeof(GIHit));
    DynamicVoxelGIRenderer* renderer = ZEN_NEW() DynamicVoxelGIRenderer(device);
    renderers.push_back(renderer);
    EXPECT_TRUE(renderer->Init(settings, true));
    EXPECT_EQ(renderer->GetCapacity(), capacity);
    return renderer;
}

SceneUniformData StaticLighting()
{
    SceneUniformData scene;
    scene.lightInfo.x              = 1;
    scene.environment              = Vec4(0);
    scene.lights[0].directionType  = Vec4(0, 0, -1, 0);
    scene.lights[0].colorIntensity = Vec4(1, 1, 1, glm::pi<float>());
    scene.lights[0].coneShadow.z   = 1;
    return scene;
}

bool FinishStatic(RenderGraph& graph, DynamicVoxelGIRenderer& renderer)
{
    const bool valid = graph.End() && device->ExecuteRenderGraph(graph);
    EXPECT_TRUE(valid) << graph.GetResult().message;
    device->FlushRHIThread();
    device->WaitForIdle();
    renderer.OnRenderGraphExecuted(valid);
    return valid;
}

template <class T> HeapVector<T> ReadStaticBuffer(RHIBuffer* buffer)
{
    HeapVector<T> result(buffer->GetRequiredSize() / sizeof(T));
    const uint8_t* data = buffer->Map();
    EXPECT_NE(data, nullptr);
    if (data != nullptr)
    {
        std::memcpy(result.data(), data, buffer->GetRequiredSize());
        buffer->Unmap();
    }
    return result;
}

StaticReadback CaptureStatic(RenderGraph& graph,
                             DynamicVoxelGIRenderer& renderer,
                             bool dynamic = false)
{
    const uint32_t size      = StaticCells * GI_FACE_COUNT * 2 * sizeof(Vec4);
    RHIBuffer* output        = Buffer(size / GI_FACE_COUNT, RHIBufferAllocateType::eGPU);
    RHIBuffer* faces         = Buffer(size, RHIBufferAllocateType::eCPURead);
    RHIBuffer* masks         = Buffer(StaticCells * 4, RHIBufferAllocateType::eCPURead);
    RHIBuffer* status        = Buffer(16, RHIBufferAllocateType::eCPURead);
    const uint32_t faceBytes =
        renderer.GetCapacity() * renderer.GetRaysPerFace() * renderer.GetCacheStride();
    RHIBuffer* hits          = Buffer(faceBytes * GI_FACE_COUNT, RHIBufferAllocateType::eCPURead);
    for (uint32_t face = 0; face < GI_FACE_COUNT; ++face)
    {
        RDGComputePassDesc capture;
        capture.SetShaderProgramName("GIStaticCaptureSP");
        capture.BindStorageImage(
            "rawIrradiance",
            (dynamic ? renderer.GetDynamicIrradiance(face) : renderer.GetRawIrradiance(face))
                ->GetDefaultView());
        capture.BindStorageImage(
            "paddedIrradiance",
            (dynamic ? renderer.GetDynamicIrradiance(face) : renderer.GetPaddedIrradiance(face))
                ->GetDefaultView());
        capture.BindStorageBuffer("StaticCapture", output, RDGContentGuarantee::eFullWrite);
        graph.AddComputePass(std::move(capture)).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
            encoder.SetPushConstants(glm::uvec2(0, StaticCells));
            encoder.Dispatch(StaticCells / GI_QUERY_GROUP_SIZE, 1, 1);
        });
        graph.AddTransferPass("ReadStaticFace")
            .CopyBuffer(output, faces, {0, size / GI_FACE_COUNT * face, size / GI_FACE_COUNT})
            .NeverCull();
        graph.AddTransferPass("ReadStaticHits")
            .CopyBuffer(renderer.GetCache(face), hits, {0, faceBytes * face, faceBytes})
            .NeverCull();
    }
    graph.AddTransferPass("ReadStaticLighting")
        .CopyBuffer(renderer.GetStatus(), status, {0, 0, 16})
        .CopyBuffer(dynamic ? renderer.GetDynamicLightMask() : renderer.GetLightMask(), masks,
                    {0, 0, StaticCells * 4})
        .NeverCull();
    StaticReadback result;
    if (FinishStatic(graph, renderer))
    {
        result.faces  = ReadStaticBuffer<Vec4>(faces);
        result.status = ReadStaticBuffer<glm::uvec4>(status)[0];
        result.masks  = ReadStaticBuffer<uint32_t>(masks);
        if (renderer.GetCacheStride() == GI_COMPACT_HIT_BYTES)
        {
            result.compactHits = ReadStaticBuffer<glm::uvec2>(hits);
        }
        else
        {
            result.hits = ReadStaticBuffer<GIHit>(hits);
        }
    }
    return result;
}

DeterministicGIProvider ReferenceStatic(RenderGraph& graph,
                                        const HeapVector<GIHit>& responses,
                                        uint64_t generation)
{
    RHIBuffer* buffer = Buffer(static_cast<uint32_t>(responses.size() * sizeof(GIHit)),
                               RHIBufferAllocateType::eCPUWrite, responses.data());
    graph.GetResourceManager()->ImportHostWrittenBuffer(buffer);
    DeterministicGIProvider provider;
    EXPECT_TRUE(provider.Prepare(buffer, static_cast<uint32_t>(responses.size()), generation,
                                 device->GetGPUInfo()));
    return provider;
}

HeapVector<Vec4> ProbeStatic(RenderGraph& graph,
                             DynamicVoxelGIRenderer& renderer,
                             const HeapVector<IrradianceProbe>& probes)
{
    const uint32_t count = static_cast<uint32_t>(probes.size());
    RHIBuffer* input =
        Buffer(count * sizeof(IrradianceProbe), RHIBufferAllocateType::eCPUWrite, probes.data());
    RHIBuffer* output   = Buffer(count * sizeof(Vec4), RHIBufferAllocateType::eGPU);
    RHIBuffer* readback = Buffer(count * sizeof(Vec4), RHIBufferAllocateType::eCPURead);
    graph.GetResourceManager()->ImportHostWrittenBuffer(input);
    RDGComputePassDesc pass;
    pass.SetShaderProgramName("GIStaticProbeSP");
    renderer.BindLightingInputs(pass);
    pass.BindStorageBuffer("Probes", input);
    pass.BindStorageBuffer("ProbeResults", output, RDGContentGuarantee::eFullWrite);
    graph.AddComputePass(std::move(pass)).RecordPassCommands([count](RDGPassCmdEncoder& encoder) {
        encoder.SetPushConstants(glm::uvec2(0, count));
        encoder.Dispatch((count + GI_QUERY_GROUP_SIZE - 1) / GI_QUERY_GROUP_SIZE, 1, 1);
    });
    graph.AddTransferPass("ReadIrradianceProbes")
        .CopyBuffer(output, readback, {0, 0, count * sizeof(Vec4)})
        .NeverCull();
    EXPECT_TRUE(FinishStatic(graph, renderer));
    return ReadStaticBuffer<Vec4>(readback);
}

RHITexture* CompositionTexture(RenderGraph& graph,
                               const Color& color,
                               RHITextureType type = RHITextureType::e2D,
                               uint32_t layers     = 1)
{
    RHITextureCreateInfo info;
    info.width = info.height = info.depth = 1;
    info.type                             = type;
    info.arrayLayers                      = layers;
    info.format                           = DataFormat::eR32G32B32A32SFloat;
    info.usageFlags.SetFlags(RHITextureUsageFlagBits::eSampled,
                             RHITextureUsageFlagBits::eTransferDst);
    RHITexture* texture = device->CreateTexture(info);
    textures.push_back(texture);
    graph.AddTransferPass("CompositionFixture").ClearTexture(texture, color);
    return texture;
}

HeapVector<Vec4> ComposeStatic(RenderGraph& graph,
                               DynamicVoxelGIRenderer& renderer,
                               SceneUniformData scene,
                               Vec3 position,
                               Vec3 normal)
{
    // Render the actual deferred fragment variant, at a noninteger upscale of
    // a one-texel coherent G-buffer. Only the test's fixed inputs are synthetic.
    constexpr uint32_t width = 7, height = 5, pixels = width * height;
    const uint32_t bytes = pixels * ZEN_LIGHTING_CAPTURE_BYTES_PER_PIXEL;
    RHIBuffer* capture   = Buffer(bytes, RHIBufferAllocateType::eGPU);
    RHIBuffer* surface =
        Buffer(pixels * ZEN_SURFACE_CAPTURE_BYTES_PER_PIXEL, RHIBufferAllocateType::eGPU);
    RHIBuffer* readback = Buffer(bytes, RHIBufferAllocateType::eCPURead);
    for (uint32_t index = 0; index < 2; ++index)
    {
        RDGComputePassDesc clear;
        clear.SetShaderProgramName(index == 0 ? "ClearLightingCaptureSP" : "ClearSurfaceCaptureSP");
        clear.BindStorageBuffer(index == 0 ? "LightingCapture" : "SurfaceCapture",
                                index == 0 ? capture : surface, RDGContentGuarantee::eFullWrite);
        graph.AddComputePass(std::move(clear)).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
            encoder.SetPushConstants(glm::uvec2(0, pixels));
            encoder.Dispatch(1, 1, 1);
        });
    }
    RHISampler* sampler = device->CreateSampler(RHISamplerCreateInfo{});
    scene.viewPos       = Vec4(position + normal * 10.0f, 0);
    RDGGraphicsPassDesc pass;
    pass.SetShaderProgramName("DeferredDynamicVoxelGICaptureSP");
    pass.SetPassTag("StaticCompositionFixture");
    pass.AddColorOutput(DataFormat::eR8G8B8A8UNORM, width, height, "static_composed");
    pass.SetRenderArea(0, 0, width, height);
    RHIGfxPipelineStates pipeline;
    pipeline.colorBlendState.AddAttachment();
    pipeline.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);
    pass.SetPipelineStates(pipeline);
    pass.BindSampledTexture(
        "positionMap", sampler,
        CompositionTexture(graph, Color(position.x, position.y, position.z, 1))->GetDefaultView());
    pass.BindSampledTexture(
        "normalMap", sampler,
        CompositionTexture(graph, Color(normal.x, normal.y, normal.z, 1))->GetDefaultView());
    pass.BindSampledTexture("geometricNormalMap", sampler,
                            CompositionTexture(graph, Color(0, 0, 1, 1))->GetDefaultView());
    pass.BindSampledTexture("albedoMap", sampler,
                            CompositionTexture(graph, Color(0.5f, 0.25f, 1, 1))->GetDefaultView());
    pass.BindSampledTexture("metallicRoughnessMap", sampler,
                            CompositionTexture(graph, Color(0, 0.5f, 0, 0))->GetDefaultView());
    pass.BindSampledTexture("emissiveOcclusionMap", sampler,
                            CompositionTexture(graph, Color(0, 0, 0, 1))->GetDefaultView());
    pass.BindSampledTexture("depthMap", sampler,
                            CompositionTexture(graph, Color(0.5f, 0, 0, 0))->GetDefaultView());
    RHITexture* cube = CompositionTexture(graph, Color(0, 0, 0, 0), RHITextureType::eCube, 6);
    for (const char* name : {"envIrradianceMap", "envPrefilteredMap", "skyboxMap"})
    {
        pass.BindSampledTexture(name, sampler, cube->GetDefaultView());
    }
    pass.BindSampledTexture("lutBRDFMap", sampler,
                            CompositionTexture(graph, Color(0, 0, 0, 0))->GetDefaultView());
    pass.BindSampledTexture(
        "sceneShadowMaps", sampler,
        CompositionTexture(graph, Color(1, 0, 0, 0), RHITextureType::e2D, 2)->GetDefaultView());
    RHITextureCreateInfo identityInfo;
    identityInfo.type  = RHITextureType::e2D;
    identityInfo.width = identityInfo.height = identityInfo.depth = 1;
    identityInfo.format                                           = DataFormat::eR32G32UInt;
    identityInfo.usageFlags.SetFlags(RHITextureUsageFlagBits::eSampled,
                                     RHITextureUsageFlagBits::eTransferDst);
    RHITexture* identity = device->CreateTexture(identityInfo);
    textures.push_back(identity);
    const glm::uvec2 id(1, GI_STATIC);
    RHIBuffer* idUpload = Buffer(sizeof(id), RHIBufferAllocateType::eCPUWrite, &id);
    graph.GetResourceManager()->ImportHostWrittenBuffer(idUpload);
    RHIBufferTextureCopyRegion region;
    region.textureSize = Vec3i(1);
    region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    graph.AddTransferPass("CompositionIdentity").CopyBufferToTexture(idUpload, identity, region);
    pass.BindSampledTexture("receiverMap", sampler, identity->GetDefaultView());
    pass.BindSampledTexture("voxelRadiance", sampler,
                            renderer.GetPaddedIrradiance(0)->GetDefaultView());
    pass.BindSampledTexture("voxelOpacity", sampler,
                            renderer.GetPaddedIrradiance(0)->GetDefaultView());
    const VoxelGIUniformData cone{Vec4(0, 0, 0, 1), Vec4(64, 1.0f / 64, 1, 0), Vec4(1),
                                  Vec4(1, 1, 0, 0)};
    pass.BindValue("uGISettings", cone);
    pass.BindValue("uSceneData", scene);
    pass.BindValue("uSceneShadows", SceneShadowUniformData{});
    pass.BindValue("uSurfaceLookup", glm::uvec4(width, height, 0, 0));
    pass.BindStorageBuffer("LightingCapture", capture, RDGContentGuarantee::eFullWrite);
    pass.BindStorageBuffer("SurfaceCapture", surface, RDGContentGuarantee::eFullWrite);
    renderer.BindLightingInputs(pass);
    graph.AddGraphicsPass(std::move(pass)).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
        encoder.SetPushConstants(glm::uvec2(width, height));
        encoder.Draw(3, 1);
    });
    graph.AddTransferPass("ReadStaticComposition")
        .CopyBuffer(capture, readback, {0, 0, bytes})
        .NeverCull();
    EXPECT_TRUE(FinishStatic(graph, renderer));
    return ReadStaticBuffer<Vec4>(readback);
}
