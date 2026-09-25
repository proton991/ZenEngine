void EnvironmentInputs(RenderGraph& graph, StaticFixture& fixture, const Color& color)
{
    fixture.inputs.normal        = fixture.staticVoxels.pNormal;
    fixture.inputs.dynamicNormal = fixture.dynamicVoxels.pNormal;
    fixture.inputs.environment =
        CompositionTexture(graph, color, RHITextureType::eCube, 6)->GetDefaultView();
    fixture.inputs.environmentSampler  = device->CreateSampler(RHISamplerCreateInfo{});
    fixture.inputs.environmentRevision = 1;
}

struct LightMaskReadback
{
    HeapVector<uint32_t> masks;
    FilterReadback irradiance;
};

LightMaskReadback CaptureLightMasks(RenderGraph& graph, DynamicVoxelGIRenderer& renderer)
{
    RHIBuffer* masks = Buffer(StaticCells * 8, RHIBufferAllocateType::eCPURead);
    graph.AddTransferPass("ReadBothLightMasks")
        .CopyBuffer(renderer.GetLightMask(), masks, {0, 0, StaticCells * 4})
        .CopyBuffer(renderer.GetDynamicLightMask(), masks, {0, StaticCells * 4, StaticCells * 4})
        .NeverCull();
    const FilterCapture capture = CreateFilterCapture();
    LightMaskReadback result;
    result.irradiance =
        CaptureFilterCell(graph, renderer, capture, StaticID({10, 10, 10}), GI_STATIC, 4);
    result.masks = ReadStaticBuffer<uint32_t>(masks);
    return result;
}

LightMaskReadback CheckCachedLightMasks(RenderGraph& graph,
                                        DynamicVoxelGIRenderer& cached,
                                        DynamicVoxelGIRenderer& recomputed,
                                        const StaticFixture& fixture,
                                        const GIVisibilityProvider& provider,
                                        const SceneUniformData& lighting,
                                        uint32_t updatedBits,
                                        bool shadows = true)
{
    EXPECT_TRUE(cached.BuildRenderGraph(fixture.inputs, provider, lighting, 1, shadows));
    EXPECT_EQ(cached.GetLightMaskUpdateBits(), updatedBits);
    const LightMaskReadback actual = CaptureLightMasks(graph, cached);
    EXPECT_TRUE(graph.Begin());
    // Explicitly discard the comparison renderer's publication each time, forcing
    // fresh visibility while the first renderer exercises persistent/partial masks.
    recomputed.OnRenderGraphExecuted(false);
    EXPECT_TRUE(recomputed.BuildRenderGraph(fixture.inputs, provider, lighting, 1, shadows));
    EXPECT_EQ(recomputed.GetLightMaskUpdateBits(), UINT32_MAX);
    const LightMaskReadback expected = CaptureLightMasks(graph, recomputed);
    EXPECT_TRUE(std::equal(actual.masks.begin(), actual.masks.end(), expected.masks.begin(),
                           expected.masks.end()));
    EXPECT_EQ(actual.irradiance.status, expected.irradiance.status);
    EXPECT_EQ(actual.irradiance.raw, expected.irradiance.raw);
    return actual;
}

uint32_t ReadLightMask(RenderGraph& graph,
                       DynamicVoxelGIRenderer& renderer,
                       uint32_t cell,
                       uint32_t kind = GI_STATIC)
{
    RHIBuffer* readback = Buffer(4, RHIBufferAllocateType::eCPURead);
    graph.AddTransferPass("ReadLightBits")
        .CopyBuffer(kind == GI_STATIC ? renderer.GetLightMask() : renderer.GetDynamicLightMask(),
                    readback, {cell * 4, 0, 4})
        .NeverCull();
    EXPECT_TRUE(FinishStatic(graph, renderer));
    return ReadStaticBuffer<uint32_t>(readback)[0];
}

Vec4 ReadSenderEnvironment(RenderGraph& graph,
                           DynamicVoxelGIRenderer& renderer,
                           uint32_t cell,
                           uint32_t kind)
{
    RHIBuffer* output   = Buffer(StaticCells * 32, RHIBufferAllocateType::eGPU);
    RHIBuffer* readback = Buffer(16, RHIBufferAllocateType::eCPURead);
    RDGComputePassDesc pass;
    pass.SetShaderProgramName("GIStaticCaptureSP");
    pass.BindStorageImage("rawIrradiance", renderer.GetSenderEnvironment(kind)->GetDefaultView());
    pass.BindStorageImage("paddedIrradiance",
                          renderer.GetSenderEnvironment(kind)->GetDefaultView());
    pass.BindStorageBuffer("StaticCapture", output, RDGContentGuarantee::eFullWrite);
    graph.AddComputePass(std::move(pass)).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
        encoder.SetPushConstants(glm::uvec2(0, StaticCells));
        encoder.Dispatch(StaticCells / GI_QUERY_GROUP_SIZE, 1, 1);
    });
    graph.AddTransferPass("ReadSenderSkyCell")
        .CopyBuffer(output, readback, {cell * 32, 0, 16})
        .NeverCull();
    EXPECT_TRUE(FinishStatic(graph, renderer));
    return ReadStaticBuffer<Vec4>(readback)[0];
}

GIQuery FaceOracleQuery(glm::uvec3 receiver,
                        uint32_t objectClass,
                        uint32_t face,
                        uint32_t ray,
                        uint32_t side = StaticSide)
{
    const float u   = (float(ray) + 0.5f) / GI_FACE_RAYS;
    const float phi = 2 * glm::pi<float>() * glm::fract(float(ray) * 0.6180339887498949f + 0.5f);
    Vec3 normal(0), tangent(0);
    normal[face / 2]            = face % 2 == 0 ? 1.0f : -1.0f;
    tangent[(face / 2 + 1) % 3] = 1;
    const Vec3 direction        = glm::normalize(
        std::sqrt(u) * std::cos(phi) * tangent +
        std::sqrt(u) * std::sin(phi) * glm::cross(normal, tangent) + std::sqrt(1 - u) * normal);
    return {Vec4(Vec3(receiver) + 0.5f + normal * 0.5f, 1e-4f),
            Vec4(direction, 2 * std::sqrt(3.0f) * side),
            glm::uvec4(GI_ALL, objectClass, StaticID(receiver, side), 0)};
}

float FaceHitFraction(const HeapVector<OccupiedCell>& cells,
                      glm::uvec3 receiver,
                      uint32_t kind,
                      uint32_t face,
                      uint32_t side = StaticSide)
{
    const GIGridUniform grid{Vec4(0, 0, 0, 1), glm::uvec4(side, GI_ALL, 0, 0),
                             glm::uvec4(1, 0, 0, 0)};
    uint32_t hits = 0;
    for (uint32_t ray = 0; ray < GI_FACE_RAYS; ++ray)
    {
        hits += Oracle(FaceOracleQuery(receiver, kind, face, ray, side), cells, grid).identity.x ==
                GI_HIT ?
            1 :
            0;
    }
    return float(hits) / GI_FACE_RAYS;
}
