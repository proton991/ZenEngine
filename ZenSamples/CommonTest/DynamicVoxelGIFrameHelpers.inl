StaticFixture FrameInputs(RenderGraph& graph,
                          const HeapVector<OccupiedCell>& cells,
                          uint64_t staticGeneration,
                          uint32_t side = StaticSide)
{
    StaticFixture fixture = StaticInputs(graph, cells, side);
    // Use exact UNORM code points, avoiding a half-code clear-color rounding tie.
    for (const VoxelTextures* voxels : {&fixture.staticVoxels, &fixture.dynamicVoxels})
    {
        graph.AddTransferPass("ExactFrameMaterials")
            .ClearTexture(voxels->pAlbedo, Color(64.0f / 255, 128.0f / 255, 191.0f / 255, 1))
            .ClearTexture(voxels->pNormal, Color(128.0f / 255, 128.0f / 255, 1, 0))
            .ClearTexture(voxels->pReflectance, Color(32.0f / 255, 64.0f / 255, 128.0f / 255, 1));
    }
    HeapVector<uint32_t> map(side * side * side, GI_INVALID_CELL);
    uint32_t count = 0;
    for (const OccupiedCell& cell : cells)
    {
        if (cell.objectClass == GI_DYNAMIC)
        {
            map[StaticID(cell.cell, side)] = count++;
        }
    }
    fixture.inputs.listGeneration = staticGeneration;
    fixture.inputs.dynamicOwner   = fixture.dynamicVoxels.pOwner;
    fixture.inputs.dynamicMap =
        Buffer(side * side * side * 4, RHIBufferAllocateType::eCPUWrite, map.data());
    graph.GetResourceManager()->ImportHostWrittenBuffer(fixture.inputs.dynamicMap);
    return fixture;
}

struct FrameReadback
{
    glm::uvec4 status{};
    glm::uvec4 counts{};
    HeapVector<uint32_t> staticList;
    HeapVector<uint32_t> dynamicList;
    HeapVector<glm::uvec4> arguments;
};
FrameReadback CaptureWork(RenderGraph& graph, DynamicVoxelGIRenderer& renderer)
{
    RHIBuffer* status   = Buffer(32, RHIBufferAllocateType::eCPURead);
    RHIBuffer* lists[2] = {Buffer(StaticCells * 4, RHIBufferAllocateType::eCPURead),
                           Buffer(StaticCells * 4, RHIBufferAllocateType::eCPURead)};
    RHIBuffer* arguments =
        Buffer(renderer.GetIndirectArguments()->GetRequiredSize(), RHIBufferAllocateType::eCPURead);
    graph.AddTransferPass("ReadReceiverWork")
        .CopyBuffer(renderer.GetStatus(), status, {0, 0, 16})
        .CopyBuffer(renderer.GetWorkCounts(), status, {0, 16, 16})
        .CopyBuffer(renderer.GetReceiverList(GI_STATIC), lists[0], {0, 0, StaticCells * 4})
        .CopyBuffer(renderer.GetReceiverList(GI_DYNAMIC), lists[1], {0, 0, StaticCells * 4})
        .CopyBuffer(renderer.GetIndirectArguments(), arguments,
                    {0, 0, arguments->GetRequiredSize()})
        .NeverCull();
    FrameReadback result;
    if (FinishStatic(graph, renderer))
    {
        const HeapVector<glm::uvec4> info = ReadStaticBuffer<glm::uvec4>(status);
        result.status                     = info[0];
        result.counts                     = info[1];
        result.staticList                 = ReadStaticBuffer<uint32_t>(lists[0]);
        result.dynamicList                = ReadStaticBuffer<uint32_t>(lists[1]);
        result.arguments                  = ReadStaticBuffer<glm::uvec4>(arguments);
    }
    return result;
}

void ReceiverSurface(RenderGraph& graph,
                     StaticVoxelGIInputs& inputs,
                     Vec3 position,
                     uint32_t objectClass,
                     bool background = false)
{
    inputs.viewportExtent = glm::uvec2(7, 5);
    inputs.surfaceViews[0] =
        CompositionTexture(graph, Color(position.x, position.y, position.z, 1))->GetDefaultView();
    inputs.surfaceViews[1] = CompositionTexture(graph, Color(0, 0, 1, 1))->GetDefaultView();
    inputs.surfaceViews[2] = CompositionTexture(graph, Color(0, 0, 1, 1))->GetDefaultView();
    inputs.surfaceViews[4] =
        CompositionTexture(graph, Color(background ? 1 : 0.5f, 0, 0, 0))->GetDefaultView();
    RHITextureCreateInfo info;
    info.type  = RHITextureType::e2D;
    info.width = info.height = info.depth = 1;
    info.format                           = DataFormat::eR32G32UInt;
    info.usageFlags.SetFlags(RHITextureUsageFlagBits::eSampled,
                             RHITextureUsageFlagBits::eTransferDst);
    RHITexture* texture = device->CreateTexture(info);
    textures.push_back(texture);
    const glm::uvec2 id(background ? 0 : 1, objectClass);
    RHIBuffer* upload = Buffer(sizeof(id), RHIBufferAllocateType::eCPUWrite, &id);
    graph.GetResourceManager()->ImportHostWrittenBuffer(upload);
    RHIBufferTextureCopyRegion region;
    region.textureSize = Vec3i(1);
    region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    graph.AddTransferPass("ReceiverIdentity").CopyBufferToTexture(upload, texture, region);
    inputs.surfaceViews[3] = texture->GetDefaultView();
}

Vec3 OracleGather(const HeapVector<OccupiedCell>& cells,
                  glm::uvec3 receiver,
                  uint32_t objectClass,
                  uint32_t face)
{
    Vec3 normal(0);
    normal[face / 2] = face % 2 == 0 ? 1.0f : -1.0f;
    Vec3 tangent(0);
    tangent[(face / 2 + 1) % 3] = 1;
    GIGridUniform grid{Vec4(0, 0, 0, 1), glm::uvec4(StaticSide, GI_ALL, 0, 0),
                       glm::uvec4(1, 0, 0, 0)};
    Vec3 result(0);
    for (uint32_t i = 0; i < GI_FACE_RAYS; ++i)
    {
        const float u   = (float(i) + 0.5f) / GI_FACE_RAYS;
        const float phi = 2 * glm::pi<float>() * glm::fract(float(i) * 0.6180339887498949f + 0.5f);
        const Vec3 direction = glm::normalize(
            std::sqrt(u) * std::cos(phi) * tangent +
            std::sqrt(u) * std::sin(phi) * glm::cross(normal, tangent) + std::sqrt(1 - u) * normal);
        const GIQuery query{Vec4(Vec3(receiver) + 0.5f + normal * 0.5f, 1e-4f),
                            Vec4(direction, 2 * std::sqrt(3.0f) * StaticSide),
                            glm::uvec4(GI_ALL, objectClass, StaticID(receiver), 0)};
        const GIHit hit = Oracle(query, cells, grid);
        if (hit.identity.x == GI_HIT)
        {
            const Vec3 rho = hit.identity.y == GI_STATIC ? Vec3(32, 64, 128) / 255.0f :
                                                           Vec3(64, 128, 191) * (0.96f / 255.0f);
            result += rho *
                (glm::pi<float>() * glm::normalize(Vec3(1.0f / 255, 1.0f / 255, 1)).z /
                 GI_FACE_RAYS);
        }
    }
    return result;
}

static void RecordArgumentFixture(RDGPassCmdEncoder& encoder,
                                  const HeapVector<ComputeDispatchChunk>& chunks)
{
    for (uint32_t i = 0; i < chunks.size(); ++i)
    {
        encoder.SetPushConstants(glm::uvec4(chunks[i].firstItem, chunks[i].itemCount, 0, i));
        encoder.Dispatch(1, 1, 1);
    }
}
static void RecordCoverageFixture(RDGPassCmdEncoder& encoder,
                                  const HeapVector<ComputeDispatchChunk>& chunks,
                                  RHIBuffer* arguments)
{
    for (uint32_t i = 0; i < chunks.size(); ++i)
    {
        encoder.SetPushConstants(glm::uvec4(chunks[i].firstItem, chunks[i].itemCount, 0, 0));
        encoder.DispatchIndirect(arguments, i * 16);
    }
}
void CheckIndirectCoverage(uint32_t count)
{
    constexpr uint32_t capacity  = 1025;
    RHIGPUInfo gpu               = device->GetGPUInfo();
    gpu.maxComputeWorkGroupCount = {2, 2, 1};
    HeapVector<ComputeDispatchChunk> chunks;
    uint32_t first = 0;
    while (first < capacity)
    {
        ComputeDispatchChunk chunk;
        ASSERT_TRUE(
            BuildComputeDispatchChunk(first, capacity - first, 1, gpu, chunk));
        chunks.push_back(chunk);
        first += chunk.itemCount;
    }
    RenderGraph& graph = *device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph.Begin());
    const glm::uvec4 counts(count, 0, 0, 0);
    RHIBuffer* work = Buffer(16, RHIBufferAllocateType::eCPUWrite, &counts);
    HeapVector<uint32_t> zeros(capacity, 0);
    RHIBuffer* coverage = Buffer(capacity * 4, RHIBufferAllocateType::eCPUWrite, zeros.data());
    const uint32_t argumentBytes = static_cast<uint32_t>(chunks.size()) * 16;
    RHIBufferCreateInfo indirectInfo;
    indirectInfo.size         = argumentBytes;
    indirectInfo.allocateType = RHIBufferAllocateType::eGPU;
    indirectInfo.usageFlags.SetFlags(RHIBufferUsageFlagBits::eIndirectBuffer,
                                     RHIBufferUsageFlagBits::eStorageBuffer,
                                     RHIBufferUsageFlagBits::eTransferSrcBuffer);
    RHIBuffer* arguments = device->CreateBuffer(indirectInfo);
    buffers.push_back(arguments);
    RHIBuffer* readback = Buffer(capacity * 4 + argumentBytes, RHIBufferAllocateType::eCPURead);
    graph.GetResourceManager()->ImportHostWrittenBuffer(work);
    graph.GetResourceManager()->ImportHostWrittenBuffer(coverage);
    GIStaticUniform uniform{};
    GIWorkUniform dispatch{glm::uvec4(2, 2, 1, chunks.size()), glm::uvec4(0)};
    RDGComputePassDesc generate;
    generate.SetShaderProgramName("GIFrameArgumentsSP");
    generate.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
    generate.independentDispatches = true;
    generate.BindValue("uStaticGI", uniform);
    generate.BindValue("uGIWork", dispatch);
    generate.BindStorageBuffer("GIWorkCounts", work);
    generate.BindStorageBuffer("GIArguments", arguments, RDGContentGuarantee::eFullWrite);
    graph.AddComputePass(std::move(generate))
        .RecordPassCommands(
            [chunks](RDGPassCmdEncoder& encoder) { RecordArgumentFixture(encoder, chunks); });
    RDGComputePassDesc consume;
    consume.SetShaderProgramName("GIFrameDispatchProbeSP");
    consume.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
    consume.independentDispatches = true;
    consume.BindValue("uStaticGI", uniform);
    consume.BindValue("uGIWork", dispatch);
    consume.BindStorageBuffer("GIWorkCounts", work);
    consume.BindStorageBuffer("Coverage", coverage);
    consume.UseIndirectBuffer(arguments);
    graph.AddComputePass(std::move(consume))
        .RecordPassCommands([chunks, arguments](RDGPassCmdEncoder& encoder) {
            RecordCoverageFixture(encoder, chunks, arguments);
        });
    graph.AddTransferPass("ReadIndirectCoverage")
        .CopyBuffer(coverage, readback, {0, 0, capacity * 4})
        .CopyBuffer(arguments, readback, {0, capacity * 4, argumentBytes})
        .NeverCull();
    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph));
    device->FlushRHIThread();
    device->WaitForIdle();
    const HeapVector<uint32_t> actual = ReadStaticBuffer<uint32_t>(readback);
    for (uint32_t i = 0; i < capacity; ++i)
    {
        EXPECT_EQ(actual[i], i < count ? 1 : 0) << count << ':' << i;
    }
    for (uint32_t i = 0; i < chunks.size(); ++i)
    {
        ComputeDispatchChunk expected;
        const uint32_t live = std::min(
            count > chunks[i].firstItem ? count - chunks[i].firstItem : 0, chunks[i].itemCount);
        ASSERT_TRUE(BuildComputeDispatchChunk(chunks[i].firstItem, live, 1, gpu,
                                              expected));
        for (uint32_t axis = 0; axis < 3; ++axis)
        {
            EXPECT_EQ(actual[capacity + 4 * i + axis], expected.groups[axis]);
        }
        EXPECT_EQ(actual[capacity + 4 * i + 3], live);
    }
}
