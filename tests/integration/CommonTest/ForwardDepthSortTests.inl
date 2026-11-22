TEST_P(ConeVoxelGIIntegrationTest, ForwardDepthSortPreservesUnsignedOrderAndStableTies)
{
    if (!SupportsForwardDepthSort(device->GetGPUInfo(), kForwardSortMaxKeys))
    {
        GTEST_SKIP() << "GPU stable sort is unsupported";
    }
    for (uint32_t count : {1u, 257u, 65537u, kForwardSortMaxKeys})
    {
        SCOPED_TRACE(count);
        HeapVector<glm::uvec2> keys(count);
        uint32_t               random = 0x932baf21;
        for (uint32_t index = 0; index < count; ++index)
        {
            uint32_t key = VisibilityRandom(random);
            if (index % 3u == 0)
            {
                key = index % 2u == 0 ? 0u : UINT32_MAX;
            }
            keys[index] = glm::uvec2(key, index);
        }
        const uint32_t bytes    = count * sizeof(glm::uvec2);
        RHIBuffer*     input    = Buffer(bytes, RHIBufferAllocateType::eCPUWriteGPURead, keys.data());
        RHIBuffer*     readback = Buffer(bytes, RHIBufferAllocateType::eCPURead);
        RenderGraph&   graph    = *device->GetCurrentFrameRDG();
        ASSERT_TRUE(graph.Begin());
        const RDGBuffer sorted = AddForwardDepthSort(*device, graph, graph.GetResourceManager()->ImportBuffer(input), count);
        ASSERT_TRUE(sorted);
        graph.AddTransferPass("ReadForwardSort")
            .CopyBuffer(sorted, graph.GetResourceManager()->ImportBuffer(readback), {0, 0, bytes})
            .NeverCull();
        ASSERT_TRUE(graph.End());
        ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
        device->FlushRHIThread();
        device->WaitForIdle();
        std::stable_sort(keys.begin(), keys.end(),
                         [](const glm::uvec2& left, const glm::uvec2& right) { return left.x < right.x; });
        const HeapVector<glm::uvec2> actual = ReadStaticBuffer<glm::uvec2>(readback);
        ASSERT_EQ(actual.size(), keys.size());
        for (uint32_t index = 0; index < count; ++index)
        {
            ASSERT_EQ(actual[index], keys[index]) << "sorted key " << index;
        }
    }
}
