TEST_P(ConeVoxelGIIntegrationTest, ForwardClustersKeepNearPlanesAndOddDepthEdgesConservative)
{
    if (!SupportsForwardDepthSort(device->GetGPUInfo(), 1))
    {
        GTEST_SKIP() << "GPU forward culling is unsupported";
    }
    constexpr uint32_t                     width  = 17;
    constexpr uint32_t                     height = 13;
    const float                            nan    = std::numeric_limits<float>::quiet_NaN();
    const HeapVector<ForwardClusterBounds> bounds{
        {Vec4(-.8f, -.8f, .6f, 1), Vec4(-.7f, -.7f, .7f, 1)},           // Hidden behind opaque depth.
        {Vec4(.93f, .90f, .6f, 1), Vec4(.96f, .94f, .7f, 1)},           // Last odd row/column.
        {Vec4(-.8f, -.8f, .1f, 1), Vec4(-.7f, -.7f, .2f, 1)},           // In front of opaque depth.
        {Vec4(-.8f, -.8f, -.1f, 1), Vec4(-.7f, -.7f, .7f, 1)},          // Crosses near plane.
        {Vec4(2, -.2f, .1f, 1), Vec4(3, .2f, .2f, 1)},                  // Outside the frustum.
        {Vec4(nan, -.8f, .6f, 1), Vec4(-.7f, -.7f, .7f, 1)},            // Unknown bounds.
        {Vec4(-.8f, -.8f, .250005f, 1), Vec4(-.7f, -.7f, .250005f, 1)}, // Depth roundoff margin.
        {Vec4(-1, -1, .6f, 1), Vec4(1, 1, .7f, 1)}}; // A single background pixel prevents whole-view occlusion.
    RHIBuffer* input    = Buffer(static_cast<uint32_t>(bounds.size() * sizeof(ForwardClusterBounds)),
                                 RHIBufferAllocateType::eCPUWriteGPURead, bounds.data());
    RHIBuffer* readback = Buffer(static_cast<uint32_t>(bounds.size() * sizeof(uint32_t)), RHIBufferAllocateType::eCPURead);
    for (bool hole : {false, true})
    {
        SCOPED_TRACE(hole);
        HeapVector<float> depth(width * height, .25f);
        depth.back()      = hole ? 1.0f : .25f;
        RHIBuffer* upload = Buffer(width * height * sizeof(float), RHIBufferAllocateType::eCPUWriteGPURead, depth.data());
        RHITextureCreateInfo info;
        info.type   = RHITextureType::e2D;
        info.width  = width;
        info.height = height;
        info.depth  = 1;
        info.format = DataFormat::eR32SFloat;
        info.usageFlags.SetFlags(RHITextureUsageFlagBits::eSampled, RHITextureUsageFlagBits::eTransferDst);
        RHITexture* texture = device->CreateTexture(info);
        textures.push_back(texture);
        ASSERT_NE(texture, nullptr);
        RenderGraph& graph = *device->GetCurrentFrameRDG();
        ASSERT_TRUE(graph.Begin());
        RHIBufferTextureCopyRegion region;
        region.textureSize = RHIOffset3D(width, height, 1);
        region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
        graph.AddTransferPass("UploadForwardDepthFixture").CopyBufferToTexture(upload, texture, region);
        const RDGTexture pyramid = AddForwardOpaqueHiZ(*device, graph, texture, width, height);
        const RDGBuffer  visible = AddForwardClusterVisibility(*device, graph, graph.GetResourceManager()->ImportBuffer(input),
                                                               static_cast<uint32_t>(bounds.size()), pyramid, Mat4(1.0f));
        graph.AddTransferPass("ReadForwardVisibility")
            .CopyBuffer(visible, graph.GetResourceManager()->ImportBuffer(readback), {0, 0, bounds.size() * sizeof(uint32_t)})
            .NeverCull();
        ASSERT_TRUE(graph.End());
        ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
        device->FlushRHIThread();
        device->WaitForIdle();
        const HeapVector<uint32_t> actual     = ReadStaticBuffer<uint32_t>(readback);
        const uint32_t             expected[] = {0, hole ? 1u : 0u, 1, 1, 0, 1, 1, hole ? 1u : 0u};
        ASSERT_EQ(actual.size(), bounds.size());
        for (uint32_t index = 0; index < actual.size(); ++index)
        {
            EXPECT_EQ(actual[index], expected[index]) << "cluster " << index;
        }
    }
}
