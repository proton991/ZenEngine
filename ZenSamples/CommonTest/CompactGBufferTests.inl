TEST_P(ConeVoxelGIIntegrationTest, CompactGBufferPreservesNormalsPositionsAndPlanarDerivatives)
{
    ShaderProgramManager::GetInstance().StoreProgram(
        ZEN_NEW() ComputeFileSP(device, "GBufferCheckSP", "VoxelGI/Calibration/gbuffer_check.comp.spv"));

    struct Sample
    {
        Vec4 normal;
        Vec4 pixelDepth;
    };

    const Mat4 view = glm::lookAt(Vec3(0.55f, -0.15f, 1.2f), Vec3(-0.4f, 0.2f, -0.7f), glm::normalize(Vec3(0.2f, 1.0f, 0.1f)));

    const glm::dmat4 inverseView  = glm::inverse(glm::dmat4(view));

    const Vec3 expectedDerivative = glm::normalize(Vec3(inverseView * glm::dvec4(0, 0, -1, 0)));

    // Finite perspective, infinite perspective, and orthographic, including roll and translation.
    for (uint32_t projectionType = 0; projectionType < 3; ++projectionType)
    {
        Mat4 projection = projectionType == 2 ? glm::ortho(-2.0f, 2.0f, -1.125f, 1.125f, 0.001f, 100.0f)
                                              : glm::perspective(glm::radians(70.0f), 1280.0f / 720.0f, 0.001f, 100.0f);

        if (projectionType == 1)
        {
            projection[2][2] = -1.0f;

            projection[3][2] = -0.001f;
        }

        projection[1][1]                 *= -1.0f;

        const GBufferUniformData gbuffer  = BuildGBufferUniformData(projection * view);

        for (const float distance : {0.01f, 1.0f, 10.0f})
        {
            SCOPED_TRACE(testing::Message() << "projection=" << projectionType << " distance=" << distance);

            HeapVector<Sample> samples;

            HeapVector<Vec3> expectedPositions;

            for (uint32_t index = 0; index < 64; ++index)
            {
                const float z      = 1.0f - 2.0f * (index + 0.5f) / 64.0f;

                const float radius = std::sqrt(1.0f - z * z);

                const float phi    = index * 2.39996323f;

                Vec3 normal(radius * std::cos(phi), radius * std::sin(phi), z);

                if (index < 6)
                {
                    normal            = Vec3(0);

                    normal[index / 2] = index % 2 == 0 ? 1.0f : -1.0f;
                }

                const Vec2 pixel(0.5f + 160.0f * (index % 8), 0.5f + 90.0f * (index / 8));

                const Vec2 ndc    = pixel / Vec2(1280, 720) * 2.0f - 1.0f;

                const float scale = projectionType == 2 ? 1.0f : distance;

                const Vec4 viewPosition(ndc.x * scale / projection[0][0], ndc.y * scale / projection[1][1], -distance, 1);

                const Vec4 clip = projection * viewPosition;

                samples.push_back({Vec4(normal, 0), Vec4(pixel, clip.z / clip.w, 0)});

                expectedPositions.push_back(Vec3(inverseView * glm::dvec4(viewPosition)));
            }

            const uint32_t outputBytes = 64 * 3 * sizeof(Vec4);

            RHIBuffer* input  = Buffer(static_cast<uint32_t>(samples.size() * sizeof(Sample)), RHIBufferAllocateType::eCPUWrite,
                                       samples.data());

            RHIBuffer* output = Buffer(outputBytes, RHIBufferAllocateType::eGPU);

            RHIBuffer* readback = Buffer(outputBytes, RHIBufferAllocateType::eCPURead);

            ASSERT_NE(input, nullptr);

            ASSERT_NE(output, nullptr);

            ASSERT_NE(readback, nullptr);

            RenderGraph graph("CompactGBufferPrecision");

            ASSERT_TRUE(graph.Begin());

            graph.GetResourceManager()->ImportHostWrittenBuffer(input);

            RDGComputePassDesc pass;

            pass.SetShaderProgramName("GBufferCheckSP");

            pass.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);

            pass.BindValue("uGBufferData", gbuffer);

            pass.BindStorageBuffer("GBufferSamples", input);

            pass.BindStorageBuffer("GBufferResults", output, RDGContentGuarantee::eFullWrite);

            graph.AddComputePass(std::move(pass)).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
                encoder.Dispatch(1, 1, 1);
            });

            graph.AddTransferPass("ReadGBufferPrecision").CopyBuffer(output, readback, {0, 0, outputBytes}).NeverCull();

            ASSERT_TRUE(graph.End());

            ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;

            device->FlushRHIThread();

            device->WaitForIdle();

            const HeapVector<Vec4> results = ReadStaticBuffer<Vec4>(readback);

            ASSERT_EQ(results.size(), 64u * 3);

            for (uint32_t index = 0; index < 64; ++index)
            {
                SCOPED_TRACE(index);

                // Less than 0.006 degrees of normal error after actual shader UNORM quantization.
                EXPECT_LT(glm::length(Vec3(results[index * 3]) - Vec3(samples[index].normal)), 0.0001f);

                // Forward D32 precision grows quadratically with distance at the 0.001 near plane.
                EXPECT_LT(glm::length(Vec3(results[index * 3 + 1]) - expectedPositions[index]),
                          0.0001f + distance * distance * 0.0001f);

                EXPECT_GT(glm::dot(Vec3(results[index * 3 + 2]), expectedDerivative), 0.9999f);
            }
        }
    }
}
