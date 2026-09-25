struct FilterReadback
{
    Vec4 raw{}, history{}, duplicateHistory{}, filtered{};
    glm::uvec4 metadata{}, status{};
};
struct FilterCapture
{
    RHIBuffer* scratch;
    RHIBuffer* readback;
};

FilterCapture CreateFilterCapture()
{
    return {Buffer(4 * sizeof(Vec4), RHIBufferAllocateType::eGPU),
            Buffer(sizeof(FilterReadback), RHIBufferAllocateType::eCPURead)};
}

FilterReadback CaptureFilterCell(RenderGraph& graph,
                                 DynamicVoxelGIRenderer& renderer,
                                 const FilterCapture& capture,
                                 uint32_t cell,
                                 uint32_t kind = GI_STATIC,
                                 uint32_t face = 0)
{
    RHITexture* history   = renderer.GetHistoryIrradiance(face, kind);
    RHITexture* sources[] = {kind == GI_STATIC ? renderer.GetRawIrradiance(face) :
                                                 renderer.GetDynamicIrradiance(face),
                             history,
                             kind == GI_STATIC ? renderer.GetPaddedIrradiance(face) :
                                                 renderer.GetDynamicFilteredIrradiance(face)};
    RDGComputePassDesc pass;
    pass.SetShaderProgramName("GIFilterCaptureSP");
    pass.BindStorageImage("rawIrradiance", sources[0]->GetDefaultView());
    pass.BindStorageImage("history", history->GetDefaultView());
    pass.BindStorageImage("filteredIrradiance", sources[2]->GetDefaultView());
    pass.BindStorageBuffer("FilterCapture", capture.scratch, RDGContentGuarantee::eFullWrite);
    graph.AddComputePass(std::move(pass))
        .RecordPassCommands([cell, side = history->GetWidth()](RDGPassCmdEncoder& encoder) {
            encoder.SetPushConstants(glm::uvec2(cell, side));
            encoder.Dispatch(1, 1, 1);
        });
    graph.AddTransferPass("ReadFilterCell")
        .CopyBuffer(capture.scratch, capture.readback, {0, 0, 64})
        .NeverCull();
    graph.AddTransferPass("ReadFilterValidity")
        .CopyBuffer(renderer.GetHistoryMetadata(kind), capture.readback, {cell * 16, 64, 16})
        .CopyBuffer(renderer.GetStatus(), capture.readback, {0, 80, 16})
        .NeverCull();
    EXPECT_TRUE(FinishStatic(graph, renderer));
    return ReadStaticBuffer<FilterReadback>(capture.readback)[0];
}

HeapVector<GIHit> ConstantResponses(uint32_t capacity, uint32_t senderCell)
{
    GIHit sender{};
    sender.identity =
        glm::uvec4(GI_HIT, GI_STATIC, senderCell, GI_CELL_PRECISION | GI_SURFACE_VALID);
    sender.normal              = Vec4(0, 0, 1, 0);
    sender.diffuseReflectance  = Vec4(0.25f, 0.5f, 1, 0);
    const uint32_t lightOffset = capacity * GI_FACE_RAYS * GI_FACE_COUNT;
    HeapVector<GIHit> responses(lightOffset + capacity * 8, sender);
    for (uint32_t i = lightOffset; i < responses.size(); ++i)
    {
        responses[i].identity = glm::uvec4(GI_MISS, 0, GI_INVALID_CELL, 0);
    }
    return responses;
}

void ReplaceOwner(RenderGraph& graph, RHITexture* texture, glm::uvec3 cell, uint32_t owner)
{
    RHIBuffer* upload = Buffer(4, RHIBufferAllocateType::eCPUWrite, &owner);
    graph.GetResourceManager()->ImportHostWrittenBuffer(upload);
    RHIBufferTextureCopyRegion region;
    region.textureOffset = Vec3i(cell);
    region.textureSize   = Vec3i(1);
    region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    graph.AddTransferPass("ReplaceHistoryOccupant").CopyBufferToTexture(upload, texture, region);
}

void CheckProviderSubstitution(GITemporalMode temporal, bool spatial)
{
    RenderGraph& graph = *device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph.Begin());
    const HeapVector<OccupiedCell> cells = {{{10, 10, 10}, GI_STATIC}, {{11, 10, 10}, GI_STATIC},
                                            {{10, 11, 10}, GI_STATIC}, {{10, 10, 12}, GI_STATIC},
                                            {{11, 10, 12}, GI_STATIC}, {{10, 11, 12}, GI_STATIC}};
    StaticFixture fixture                = StaticInputs(graph, cells);
    DynamicVoxelGIRenderer* renderer     = StaticRenderer(static_cast<uint32_t>(cells.size()));
    renderer->SetFiltering(temporal, spatial);
    fixture.inputs.timeSeconds = 0;
    VoxelDDAProvider dda;
    ASSERT_TRUE(dda.Prepare(fixture.staticVoxels, fixture.dynamicVoxels, fixture.inputs.grid, 1));
    const SceneUniformData lighting = StaticLighting();
    ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, dda, lighting, 1, true));
    const StaticReadback actual = CaptureStatic(graph, *renderer);
    ASSERT_EQ(actual.status.z, 0);
    EXPECT_EQ(actual.masks[fixture.ids[0]], 0); // Upper layer occludes the light.
    EXPECT_EQ(actual.masks[fixture.ids[3]], 1);
    EXPECT_GT(actual.faces[2 * (4 * StaticCells + fixture.ids[0])].z,
              0); // Upper sender illuminates lower +Z face.
    EXPECT_EQ(actual.faces[2 * (5 * StaticCells + fixture.ids[3])].z,
              0); // Unlit lower hits remain opaque.
    ASSERT_TRUE(graph.Begin());
    const HeapVector<Vec4> ddaComposition =
        ComposeStatic(graph, *renderer, lighting, Vec3(10.5f, 10.5f, 10.1f), Vec3(0, 0, 1));
    HeapVector<GIHit> responses = actual.hits;
    // Independent M1 light queries supply exact decoded responses to the replacement.
    HeapVector<GIQuery> lightQueries;
    for (const OccupiedCell& cell : cells)
    {
        for (uint32_t sample = 0; sample < 8; ++sample)
        {
            const Vec3 sign((sample & 1) ? 1 : -1, (sample & 2) ? 1 : -1, (sample & 4) ? 1 : -1);
            const Vec3 origin = Vec3(cell.cell) + 0.5f + sign * 0.475f;
            lightQueries.push_back({Vec4(origin, 1e-4f),
                                    Vec4(0, 0, 1, 2 * std::sqrt(3.0f) * StaticSide - 1e-4f),
                                    glm::uvec4(GI_STATIC, GI_STATIC, StaticID(cell.cell), 0)});
        }
    }
    ASSERT_TRUE(graph.Begin());
    const HeapVector<GIQueryResult> lightResults = RunQueries(graph, dda, lightQueries);
    for (const GIQueryResult& result : lightResults)
    {
        responses.push_back(result.closest);
    }
    ASSERT_TRUE(graph.Begin());
    const DeterministicGIProvider reference = ReferenceStatic(graph, responses, 2);
    ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, reference, lighting, 1, true));
    const StaticReadback replacement = CaptureStatic(graph, *renderer);
    EXPECT_EQ(actual.status, replacement.status);
    ASSERT_EQ(actual.masks.size(), replacement.masks.size());
    EXPECT_EQ(std::memcmp(actual.masks.data(), replacement.masks.data(), actual.masks.size() * 4),
              0);
    ASSERT_EQ(actual.faces.size(), replacement.faces.size());
    EXPECT_EQ(std::memcmp(actual.faces.data(), replacement.faces.data(),
                          actual.faces.size() * sizeof(Vec4)),
              0);
    ASSERT_TRUE(graph.Begin());
    const HeapVector<Vec4> referenceComposition =
        ComposeStatic(graph, *renderer, lighting, Vec3(10.5f, 10.5f, 10.1f), Vec3(0, 0, 1));
    ASSERT_EQ(ddaComposition.size(), referenceComposition.size());
    EXPECT_EQ(std::memcmp(ddaComposition.data(), referenceComposition.data(),
                          ddaComposition.size() * sizeof(Vec4)),
              0);
    // Same production trilinear/normal-weighting helper is used by deferred composition.
    const HeapVector<IrradianceProbe> probes = {
        {Vec4(10.5f, 10.5f, 10.1f, 0), Vec4(0, 0, 1, 0)},
        {Vec4(10.1f, 10.2f, 10.3f, 0), Vec4(glm::normalize(Vec3(1, 2, 3)), 0)}};
    ASSERT_TRUE(graph.Begin());
    const HeapVector<Vec4> referenceValues = ProbeStatic(graph, *renderer, probes);
    ASSERT_TRUE(graph.Begin());
    ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, dda, lighting, 1, true));
    const HeapVector<Vec4> ddaValues = ProbeStatic(graph, *renderer, probes);
    ASSERT_EQ(referenceValues.size(), ddaValues.size());
    EXPECT_EQ(
        std::memcmp(referenceValues.data(), ddaValues.data(), ddaValues.size() * sizeof(Vec4)), 0);
    // Invalid/truncated queries cannot become a lit sky miss.
    GIGridUniform incomplete = fixture.inputs.grid;
    incomplete.dimensions.y  = GI_DYNAMIC;
    ASSERT_TRUE(dda.Prepare(fixture.staticVoxels, fixture.dynamicVoxels, incomplete, 3));
    ASSERT_TRUE(graph.Begin());
    ASSERT_TRUE(renderer->BuildRenderGraph(fixture.inputs, dda, lighting, 1, true));
    const StaticReadback unknown = CaptureStatic(graph, *renderer);
    EXPECT_NE(unknown.status.z & GI_STATIC_UNKNOWN, 0);
}
