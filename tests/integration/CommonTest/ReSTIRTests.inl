TEST_P(ConeVoxelGIIntegrationTest, ReSTIRStreamingPackingJacobianAndEmissiveDisk)
{
    ShaderProgramManager::GetInstance().StoreProgram(
        ZEN_NEW() ComputeFileSP(device, "ReSTIRCheckSP", "VoxelGI/Calibration/restir_check.comp.spv"));
    constexpr uint32_t count    = 16384;
    constexpr uint32_t bytes    = count * sizeof(Vec4);
    RHIBuffer*         output   = Buffer(bytes, RHIBufferAllocateType::eGPU);
    RHIBuffer*         readback = Buffer(bytes, RHIBufferAllocateType::eCPURead);
    RenderGraph        graph("restir_estimator_contract");
    ASSERT_TRUE(graph.Begin());
    RDGComputePassDesc pass;
    pass.SetShaderProgramName("ReSTIRCheckSP");
    pass.BindStorageBuffer("Results", output, RDGContentGuarantee::eFullWrite);
    graph.AddComputePass(std::move(pass)).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
        encoder.Dispatch(count / 64, 1, 1);
    });
    graph.AddTransferPass("ReadReservoirChecks").CopyBuffer(output, readback, {0, 0, bytes}).NeverCull();
    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
    device->FlushRHIThread();
    device->WaitForIdle();
    const HeapVector<Vec4> result = ReadStaticBuffer<Vec4>(readback);
    ASSERT_EQ(result.size(), count);
    EXPECT_EQ(result[0], Vec4(0.5f, 4, 128, 127));
    EXPECT_NEAR(result[1].x, 0.25f, 1e-6f);
    EXPECT_NEAR(result[1].y, 4.0f, 1e-6f);
    EXPECT_EQ(result[1].z, 0.0f);
    EXPECT_NEAR(result[1].w, 1.0f / std::sqrt(8.0f), 1e-6f);
    EXPECT_EQ(Vec3(result[2]), Vec3(1, 127, 1));
    EXPECT_GT(result[2].w, 0.99999f);
    EXPECT_EQ(result[3], Vec4(3, 4, 0, 0));
    EXPECT_EQ(result[4], Vec4(1, 0, 0, 0));
    EXPECT_EQ(result[5], Vec4(2, 2, 2, 4));
    EXPECT_EQ(result[6], Vec4(0, 1, 65408, 0));
    EXPECT_GT(result[7].x, 0.99999f);
    EXPECT_EQ(result[7].y, 1.0f);
    double mean = 0;
    for (uint32_t i = 8; i < count; ++i)
    {
        ASSERT_TRUE(std::isfinite(result[i].x));
        EXPECT_EQ(result[i].w, 64.0f);
        mean += result[i].x;
    }
    EXPECT_NEAR(mean / (count - 8), 2.0, 0.01);
}

TEST_P(ConeVoxelGIIntegrationTest, ReSTIRGGXLobePreservesNormalizedReflectionEnergy)
{
    ShaderProgramManager::GetInstance().StoreProgram(
        ZEN_NEW() ComputeFileSP(device, "ReSTIRLobeIntegralSP", "VoxelGI/restir_lobe_integral.comp.spv"));
    ShaderProgramManager::GetInstance().StoreProgram(
        ZEN_NEW() ComputeFileSP(device, "ReSTIRLobeCheckSP", "VoxelGI/Calibration/restir_lobe_check.comp.spv"));
    RHITextureCreateInfo info;
    info.type  = RHITextureType::e2D;
    info.width = info.height = 128;
    info.depth               = 1;
    info.format              = DataFormat::eR32SFloat;
    info.usageFlags.SetFlags(RHITextureUsageFlagBits::eSampled, RHITextureUsageFlagBits::eStorage);
    RHITexture* integral = device->CreateTexture(info);
    textures.push_back(integral);
    RHISampler* sampler  = device->CreateSampler({});
    RHIBuffer*  output   = Buffer(16 * sizeof(Vec4), RHIBufferAllocateType::eGPU);
    RHIBuffer*  readback = Buffer(16 * sizeof(Vec4), RHIBufferAllocateType::eCPURead);
    RenderGraph graph("restir_normalized_lobe");
    ASSERT_TRUE(graph.Begin());
    RDGComputePassDesc build;
    build.SetShaderProgramName("ReSTIRLobeIntegralSP");
    build.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
    build.BindStorageImage("lobeIntegral", integral->GetDefaultView(), RDGContentGuarantee::eFullWrite);
    graph.AddComputePass(std::move(build)).RecordPassCommands([](RDGPassCmdEncoder& encoder) { encoder.Dispatch(16, 16, 1); });
    RDGComputePassDesc check;
    check.SetShaderProgramName("ReSTIRLobeCheckSP");
    check.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
    check.BindValue("uReSTIRData", ReSTIRUniformData{});
    check.BindSampledTexture("restirLobeIntegral", sampler, integral->GetDefaultView());
    check.BindStorageBuffer("Results", output, RDGContentGuarantee::eFullWrite);
    graph.AddComputePass(std::move(check)).RecordPassCommands([](RDGPassCmdEncoder& encoder) { encoder.Dispatch(16, 1, 1); });
    graph.AddTransferPass("ReadLobeIntegral").CopyBuffer(output, readback, {0, 0, 16 * sizeof(Vec4)}).NeverCull();
    ASSERT_TRUE(graph.End());
    ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
    device->FlushRHIThread();
    device->WaitForIdle();
    const HeapVector<Vec4> results = ReadStaticBuffer<Vec4>(readback);
    ASSERT_EQ(results.size(), 16u);
    for (uint32_t i = 0; i < 16; ++i)
    {
        SCOPED_TRACE(i);
        EXPECT_NEAR(results[i].x, 1.0f, 0.02f);
    }
    // At alpha=1 and normal incidence, integral f*cos = integral_0^1 mu/(1+mu) dmu.
    EXPECT_NEAR(results[15].y, 1.0f - std::log(2.0f), 0.005f);
}
