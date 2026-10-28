// Optional GPU field readback, included after DistanceFieldSceneTests.inl.
// No renderer state or production completion policy is changed by these diagnostics.
struct FieldDiagnosticSample
{
    Vec4       positionDistance;
    Vec4       normal;
    glm::uvec4 storage;
};
static_assert(sizeof(FieldDiagnosticSample) == 48);

HeapVector<FieldDiagnosticSample> SampleBuiltField(RenderDevice&           device,
                                                   DistanceFieldScene&     field,
                                                   const HeapVector<Vec4>& points)
{
    HeapVector<FieldDiagnosticSample> samples;
    ShaderProgramManager::GetInstance().StoreProgram(
        ZEN_NEW() ComputeFileSP(&device, "DistanceFieldSampleSP", "VoxelGI/Calibration/distance_field_sample.comp.spv"));
    RHIBufferCreateInfo info;
    info.size = points.size() * sizeof(Vec4);
    info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eStorageBuffer);
    info.allocateType = RHIBufferAllocateType::eCPUWrite;
    RHIBuffer* input  = device.CreateBuffer(info);
    info.size         = points.size() * sizeof(FieldDiagnosticSample);
    info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferSrcBuffer);
    info.allocateType   = RHIBufferAllocateType::eGPU;
    RHIBuffer* output   = device.CreateBuffer(info);
    info.usageFlags     = BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eTransferDstBuffer);
    info.allocateType   = RHIBufferAllocateType::eCPURead;
    RHIBuffer* readback = device.CreateBuffer(info);
    bool       valid    = input != nullptr && output != nullptr && readback != nullptr;
    uint8_t*   mapped   = valid ? input->Map() : nullptr;
    valid               = valid && mapped != nullptr;
    if (valid)
    {
        std::memcpy(mapped, points.data(), points.size() * sizeof(Vec4));
        input->Unmap();
        RenderGraph& graph = *device.GetCurrentFrameRDG();
        valid              = graph.Begin();
        if (valid)
        {
            graph.GetResourceManager()->ImportHostWrittenBuffer(input);
            RDGComputePassDesc pass;
            pass.SetShaderProgramName("DistanceFieldSampleSP");
            pass.SetPassTag("DistanceFieldDiagnosticSample");
            pass.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
            valid = field.BindTraceInputs(pass);
            if (valid)
            {
                pass.BindStorageBuffer("DistanceFieldSamplePoints", input);
                pass.BindStorageBuffer("DistanceFieldSampleRecords", output, RDGContentGuarantee::eFullWrite);
                const uint32_t groups = (static_cast<uint32_t>(points.size()) + 63u) / 64u;
                graph.AddComputePass(std::move(pass)).RecordPassCommands([groups](RDGPassCmdEncoder& encoder) {
                    encoder.Dispatch(groups, 1, 1);
                });
                graph.AddTransferPass("ReadDistanceFieldDiagnostic")
                    .CopyBuffer(output, readback, {0, 0, info.size})
                    .NeverCull();
            }
            valid = graph.End() && valid;
            if (valid)
            {
                valid = device.ExecuteRenderGraph(graph);
            }
            EXPECT_TRUE(valid) << graph.GetResult().message;
            // Test-only readback, matching TraceBuiltField's explicit completion boundary.
            device.FlushRHIThread();
            device.WaitForIdle();
        }
        mapped = valid ? readback->Map() : nullptr;
        valid  = valid && mapped != nullptr;
        if (valid)
        {
            samples.resize(points.size());
            std::memcpy(samples.data(), mapped, samples.size() * sizeof(FieldDiagnosticSample));
            readback->Unmap();
        }
    }
    EXPECT_TRUE(valid);
    device.DestroyBuffer(input);
    device.DestroyBuffer(output);
    device.DestroyBuffer(readback);
    return samples;
}

struct FieldSliceDescription
{
    Vec3     minimum;
    Vec3     maximum;
    Vec3     center;
    Mat4     transform{1};
    float    metresPerUnit{1};
    uint32_t pose{0};
    uint32_t mode{0};
};

bool WriteBuiltFieldSlices(RenderDevice&                device,
                           DistanceFieldScene&          field,
                           const std::string&           prefix,
                           const FieldSliceDescription& description)
{
    constexpr uint32_t               width = 256, height = 512, planes = 2;
    const DistanceFieldSceneUniform& uniform = field.GetUniform();
    const Vec3                       minimum = description.minimum - Vec3(2.0f * uniform.gridMinCellSize.w);
    const Vec3                       maximum = description.maximum + Vec3(2.0f * uniform.gridMinCellSize.w);
    HeapVector<Vec4>                 points;
    points.reserve(planes * width * height);
    for (uint32_t plane = 0; plane < planes; ++plane)
    {
        const uint32_t axis = plane == 0 ? 0 : 2;
        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                Vec3 point  = description.center;
                point[axis] = glm::mix(minimum[axis], maximum[axis], static_cast<float>(x) / (width - 1u));
                point.y     = glm::mix(minimum.y, maximum.y, static_cast<float>(y) / (height - 1u));
                points.push_back(Vec4(point, 1));
            }
        }
    }
    HeapVector<FieldDiagnosticSample> samples = SampleBuiltField(device, field, points);
    bool                              valid   = samples.size() == points.size();
    for (FieldDiagnosticSample& sample : samples)
    {
        // Preserve infinity/NaN for cells without a stored distance. Availability is explicit.
        sample.positionDistance *= description.metresPerUnit;
    }
    const uint32_t header[] = {
        0x5344465au, 1u, width, height, planes, sizeof(FieldDiagnosticSample), static_cast<uint32_t>(samples.size()), 0u};
    FILE* binary = valid ? std::fopen((prefix + ".bin").c_str(), "wb") : nullptr;
    valid        = valid && binary != nullptr;
    if (binary != nullptr)
    {
        valid = std::fwrite(header, sizeof(header), 1, binary) == 1
             && std::fwrite(samples.data(), sizeof(FieldDiagnosticSample), samples.size(), binary) == samples.size();
        valid = std::fclose(binary) == 0 && valid;
    }
    FILE* metadata = valid ? std::fopen((prefix + ".json").c_str(), "wb") : nullptr;
    valid          = valid && metadata != nullptr;
    if (metadata != nullptr)
    {
        const float scale = description.metresPerUnit;
        valid =
            std::fprintf(
                metadata,
                "{\n  \"format\": \"zen-distance-field-slices-v1\",\n  \"width\": %u, \"height\": %u, \"planes\": %u,\n"
                "  \"pose\": %u, \"mode\": %u, \"metres_per_unit\": %.9g,\n"
                "  \"grid_min_world\": [%.9g, %.9g, %.9g], \"cells_per_axis\": %u,\n"
                "  \"brick_extent_world\": %.9g, \"spacing_metres\": %.9g,\n"
                "  \"hit_tolerance_brick_units\": %.9g, \"effective_hit_band_metres\": %.9g,\n"
                "  \"minimum_metres\": [%.9g, %.9g, %.9g], \"maximum_metres\": [%.9g, %.9g, %.9g],\n"
                "  \"center_metres\": [%.9g, %.9g, %.9g],\n"
                "  \"record_layout\": \"32-byte uint32 header; plane-major, y-major, x-minor rows of 48 bytes: "
                "float32 position_xyz_metres,distance_metres; float32 raw_gradient_xyzw; uint32 state,brick,gradient_valid,samples\",\n"
                "  \"states\": {\"0\": \"certified_empty_no_distance\", \"1\": \"stored_trilinear_distance\", "
                "\"2\": \"unavailable\", \"3\": \"outside_level_no_distance\"},\n"
                "  \"planes_description\": \"front XY at center.z; side ZY at center.x\",\n"
                "  \"gradient_semantics\": \"raw unoriented brick-local field derivative; not trace-facing normal\",\n"
                "  \"transform_column_major\": [",
                width, height, planes, description.pose, description.mode, scale, uniform.gridMinCellSize.x,
                uniform.gridMinCellSize.y, uniform.gridMinCellSize.z, uniform.dimensions.x, uniform.gridMinCellSize.w,
                uniform.gridMinCellSize.w * scale / 7.0f, uniform.tracing.x,
                2.0f * uniform.tracing.x * uniform.gridMinCellSize.w * scale, minimum.x * scale, minimum.y * scale,
                minimum.z * scale, maximum.x * scale, maximum.y * scale, maximum.z * scale, description.center.x * scale,
                description.center.y * scale, description.center.z * scale)
            > 0;
        for (uint32_t component = 0; valid && component < 16; ++component)
        {
            valid = std::fprintf(metadata, "%s%.9g", component == 0 ? "" : ", ",
                                 description.transform[component / 4u][component % 4u])
                  > 0;
        }
        valid = std::fprintf(metadata, "]\n}\n") > 0 && valid;
        valid = std::fclose(metadata) == 0 && valid;
    }
    return valid;
}

TEST_P(DistanceFieldSceneIntegrationTest, DistanceFieldDiagnosticSamplesReadConstructedAtlas)
{
    RegisterDistanceFieldPrograms(device);
    BuiltFieldSceneFixture fixture;
    ASSERT_TRUE(fixture.Init(device, false));
    DistanceFieldScene field(device, SmallFieldSettings());
    ASSERT_TRUE(CompleteBuiltField(*device, *fixture.scene, field));
    ASSERT_EQ(field.GetState(), DistanceFieldScene::State::Ready);
    const DistanceFieldSceneUniform& uniform = field.GetUniform();
    const HeapVector<Vec4>           points  = {Vec4(0.75f, 0, 0, 1), Vec4(0.75f - 0.3f * uniform.gridMinCellSize.w, 0, 0, 1),
                                                Vec4(Vec3(uniform.gridMinCellSize) - Vec3(1), 1)};
    const HeapVector<FieldDiagnosticSample> samples = SampleBuiltField(*device, field, points);
    ASSERT_EQ(samples.size(), points.size());
    uint32_t stored = 0, unavailable = 0;
    for (uint32_t index = 0; index < samples.size(); ++index)
    {
        EXPECT_EQ(Vec3(samples[index].positionDistance), Vec3(points[index]));
        stored      += samples[index].storage.x == 1u ? 1u : 0u;
        unavailable += samples[index].storage.x == 2u ? 1u : 0u;
        if (samples[index].storage.x == 1u)
        {
            EXPECT_TRUE(std::isfinite(samples[index].positionDistance.w));
            EXPECT_GE(samples[index].positionDistance.w, 0.0f);
        }
    }
    EXPECT_EQ(stored, 2u);
    EXPECT_EQ(unavailable, 0u);
    EXPECT_GT(samples[1].positionDistance.w, 0.0f);
    EXPECT_EQ(samples[2].storage.x, 3u);
    EXPECT_TRUE(std::isinf(samples[2].positionDistance.w));
    field.Destroy();
}
