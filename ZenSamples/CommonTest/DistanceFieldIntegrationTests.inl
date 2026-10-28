// This fixture creates analytic distances on the CPU only to isolate the native GPU tracer.
// Production field construction must consume committed scene geometry on the GPU.
struct DistanceFieldCheckUniform
{
    Vec4       gridMinCellSize;
    glm::uvec4 dimensions;
    glm::uvec4 atlasDimensions;
    Vec4       tracing;
};

struct DistanceFieldCheckRay
{
    Vec4       originMinimum;
    Vec4       directionMaximum;
    glm::uvec4 limits{0u};
};

struct DistanceFieldCheckResult
{
    Vec4       positionDistance;
    Vec4       normal;
    glm::uvec4 statistics;
};

static_assert(sizeof(DistanceFieldCheckUniform) == 64u);
static_assert(sizeof(DistanceFieldCheckRay) == 48u);
static_assert(sizeof(DistanceFieldCheckResult) == 48u);

struct DistanceFieldCheckBox
{
    Vec3 minimum;
    Vec3 maximum;
};

struct DistanceFieldCheckFixture
{
    DistanceFieldCheckUniform uniform{};
    HeapVector<uint32_t>      cells;
    HeapVector<uint32_t>      hierarchy;
    HeapVector<uint8_t>       atlas;
};

constexpr uint32_t FieldCoveredMiss = 0u;
constexpr uint32_t FieldHit         = 1u;
constexpr uint32_t FieldUnavailable = 2u;
constexpr uint32_t FieldUnknownCell = 0xffffffffu;

float AnalyticFieldDistance(const Vec3& position, const HeapVector<DistanceFieldCheckBox>& boxes, float plane)
{
    float distance = std::isfinite(plane) ? std::abs(position.x - plane) : std::numeric_limits<float>::max();
    for (const DistanceFieldCheckBox& box : boxes)
    {
        const Vec3  center         = (box.minimum + box.maximum) * 0.5f;
        const Vec3  halfExtent     = (box.maximum - box.minimum) * 0.5f;
        const Vec3  q              = glm::abs(position - center) - halfExtent;
        const float signedDistance = glm::length(glm::max(q, Vec3(0.0f))) + std::min(std::max(q.x, std::max(q.y, q.z)), 0.0f);
        distance                   = std::min(distance, std::abs(signedDistance));
    }
    return distance;
}

void BuildFieldCheckHierarchy(DistanceFieldCheckFixture& fixture)
{
    uint32_t side   = fixture.uniform.dimensions.x;
    uint32_t offset = 0u;
    fixture.hierarchy.resize(fixture.cells.size());
    for (uint32_t index = 0u; index < fixture.cells.size(); ++index)
    {
        fixture.hierarchy[index] = fixture.cells[index] == FieldUnknownCell ? 2u : (fixture.cells[index] != 0u ? 1u : 0u);
    }
    while (side > 1u)
    {
        const uint32_t parentSide   = side / 2u;
        const uint32_t parentOffset = static_cast<uint32_t>(fixture.hierarchy.size());
        fixture.hierarchy.resize(parentOffset + parentSide * parentSide * parentSide);
        for (uint32_t z = 0u; z < side; ++z)
        {
            for (uint32_t y = 0u; y < side; ++y)
            {
                for (uint32_t x = 0u; x < side; ++x)
                {
                    const uint32_t child       = offset + x + side * (y + side * z);
                    const uint32_t parent      = parentOffset + x / 2u + parentSide * (y / 2u + parentSide * (z / 2u));
                    fixture.hierarchy[parent] |= fixture.hierarchy[child];
                }
            }
        }
        offset = parentOffset;
        side   = parentSide;
    }
}

DistanceFieldCheckFixture BuildFieldCheckFixture(uint32_t                                 side,
                                                 float                                    extent,
                                                 const HeapVector<DistanceFieldCheckBox>& boxes,
                                                 float plane = std::numeric_limits<float>::quiet_NaN())
{
    DistanceFieldCheckFixture fixture;
    const float               cellExtent = extent / static_cast<float>(side);
    uint32_t                  levels     = 1u;
    for (uint32_t levelSide = side; levelSide > 1u; levelSide /= 2u)
    {
        ++levels;
    }
    fixture.uniform.gridMinCellSize = Vec4(0, 0, 0, cellExtent);
    fixture.uniform.dimensions      = glm::uvec4(side, 0, levels, 2048u);
    // One R8 quantization step is a fixture setting, not an AMD traversal default.
    fixture.uniform.tracing = Vec4(1.0f / 255.0f, 64, 0, 0);
    fixture.cells.resize(side * side * side);
    uint32_t brickCount = 0u;
    for (uint32_t z = 0u; z < side; ++z)
    {
        for (uint32_t y = 0u; y < side; ++y)
        {
            for (uint32_t x = 0u; x < side; ++x)
            {
                const Vec3 center = (Vec3(x, y, z) + 0.5f) * cellExtent;
                // Conservative analytic allocation: a surface crossing this cube lies within its half diagonal.
                // The extra half diagonal also supplies samples near cell faces without turning empty cells solid.
                if (AnalyticFieldDistance(center, boxes, plane) <= std::sqrt(3.0f) * cellExtent)
                {
                    fixture.cells[x + side * (y + side * z)] = ++brickCount;
                }
            }
        }
    }
    uint32_t tiles = 1u;
    while (tiles * tiles * tiles < brickCount)
    {
        ++tiles;
    }
    const uint32_t atlasSide        = tiles * 8u;
    fixture.uniform.dimensions.y    = brickCount;
    fixture.uniform.atlasDimensions = glm::uvec4(atlasSide, atlasSide, atlasSide, 0u);
    fixture.atlas.resize(atlasSide * atlasSide * atlasSide);
    std::fill(fixture.atlas.begin(), fixture.atlas.end(), uint8_t(255u));
    for (uint32_t z = 0u; z < side; ++z)
    {
        for (uint32_t y = 0u; y < side; ++y)
        {
            for (uint32_t x = 0u; x < side; ++x)
            {
                const uint32_t value = fixture.cells[x + side * (y + side * z)];
                if (value != 0u)
                {
                    const uint32_t   brick = value - 1u;
                    const glm::uvec3 tile(brick % tiles, (brick / tiles) % tiles, brick / (tiles * tiles));
                    for (uint32_t sz = 0u; sz < 8u; ++sz)
                    {
                        for (uint32_t sy = 0u; sy < 8u; ++sy)
                        {
                            for (uint32_t sx = 0u; sx < 8u; ++sx)
                            {
                                const Vec3       position = (Vec3(x, y, z) + Vec3(sx, sy, sz) / 7.0f) * cellExtent;
                                const float      distance = AnalyticFieldDistance(position, boxes, plane) / cellExtent;
                                const glm::uvec3 texel    = tile * 8u + glm::uvec3(sx, sy, sz);
                                const uint32_t   index    = texel.x + atlasSide * (texel.y + atlasSide * texel.z);
                                fixture.atlas[index] =
                                    static_cast<uint8_t>(std::lround(glm::clamp(distance, 0.0f, 1.0f) * 255.0f));
                            }
                        }
                    }
                }
            }
        }
    }
    BuildFieldCheckHierarchy(fixture);
    return fixture;
}

class DistanceFieldIntegrationTest : public ConeVoxelGIIntegrationTest
{
protected:
    void SlitExperiment(const HeapVector<uint32_t>&   resolutions,
                        const HeapVector<float>&      tolerances,
                        const HeapVector<glm::uvec2>& phases,
                        bool                          recordStorage);

    void Trace(const DistanceFieldCheckFixture&         fixture,
               const HeapVector<DistanceFieldCheckRay>& rays,
               HeapVector<DistanceFieldCheckResult>&    results)
    {
        ASSERT_FALSE(rays.empty());
        const size_t         firstBuffer  = buffers.size();
        const size_t         firstTexture = textures.size();
        RHITextureCreateInfo info;
        info.type   = RHITextureType::e3D;
        info.width  = fixture.uniform.atlasDimensions.x;
        info.height = fixture.uniform.atlasDimensions.y;
        info.depth  = fixture.uniform.atlasDimensions.z;
        info.format = DataFormat::eR8UNORM;
        info.usageFlags.SetFlags(RHITextureUsageFlagBits::eSampled, RHITextureUsageFlagBits::eTransferDst);
        RHITexture* atlas = device->CreateTexture(info);
        ASSERT_NE(atlas, nullptr);
        textures.push_back(atlas);
        const uint32_t count = static_cast<uint32_t>(rays.size());
        const uint32_t bytes = count * sizeof(DistanceFieldCheckResult);
        RHIBuffer*     upload =
            Buffer(static_cast<uint32_t>(fixture.atlas.size()), RHIBufferAllocateType::eCPUWrite, fixture.atlas.data());
        RHIBuffer*   cells     = Buffer(static_cast<uint32_t>(fixture.cells.size() * sizeof(uint32_t)),
                                        RHIBufferAllocateType::eCPUWrite, fixture.cells.data());
        RHIBuffer*   hierarchy = Buffer(static_cast<uint32_t>(fixture.hierarchy.size() * sizeof(uint32_t)),
                                        RHIBufferAllocateType::eCPUWrite, fixture.hierarchy.data());
        RHIBuffer*   input     = Buffer(count * sizeof(DistanceFieldCheckRay), RHIBufferAllocateType::eCPUWrite, rays.data());
        RHIBuffer*   output    = Buffer(bytes, RHIBufferAllocateType::eGPU);
        RHIBuffer*   readback  = Buffer(bytes, RHIBufferAllocateType::eCPURead);
        RenderGraph& graph     = *device->GetCurrentFrameRDG();
        ASSERT_TRUE(graph.Begin());
        graph.GetResourceManager()->ImportHostWrittenBuffer(upload);
        graph.GetResourceManager()->ImportHostWrittenBuffer(cells);
        graph.GetResourceManager()->ImportHostWrittenBuffer(hierarchy);
        graph.GetResourceManager()->ImportHostWrittenBuffer(input);
        RHIBufferTextureCopyRegion region;
        region.textureSize = Vec3i(info.width, info.height, info.depth);
        region.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
        graph.AddTransferPass("DistanceFieldFixtureUpload").CopyBufferToTexture(upload, atlas, region);
        RHISamplerCreateInfo sampler;
        sampler.minFilter = RHISamplerFilter::eLinear;
        sampler.magFilter = RHISamplerFilter::eLinear;
        sampler.maxLod    = 0.0f;
        RDGComputePassDesc pass;
        pass.SetShaderProgramName("DistanceFieldCheckSP");
        pass.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
        pass.BindValue("uDistanceFieldData", fixture.uniform);
        pass.BindStorageBuffer("DistanceFieldCells", cells);
        pass.BindStorageBuffer("DistanceFieldHierarchy", hierarchy);
        pass.BindSampledTexture("distanceFieldAtlas", device->CreateSampler(sampler), atlas->GetDefaultView());
        pass.BindStorageBuffer("DistanceFieldCheckRays", input);
        pass.BindStorageBuffer("DistanceFieldCheckResults", output, RDGContentGuarantee::eFullWrite);
        graph.AddComputePass(std::move(pass)).RecordPassCommands([count](RDGPassCmdEncoder& encoder) {
            encoder.Dispatch((count + 63u) / 64u, 1, 1);
        });
        graph.AddTransferPass("DistanceFieldFixtureReadback").CopyBuffer(output, readback, {0, 0, bytes}).NeverCull();
        ASSERT_TRUE(graph.End());
        ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
        device->FlushRHIThread();
        device->WaitForIdle();
        results = ReadStaticBuffer<DistanceFieldCheckResult>(readback);
        // The test already waited for completion. Release graph references and retire only this
        // dispatch's resources so larger sweeps do not retain every atlas until suite teardown.
        ASSERT_TRUE(graph.Reset());
        for (size_t index = firstBuffer; index < buffers.size(); ++index)
        {
            device->DestroyBuffer(buffers[index]);
        }
        buffers.resize(firstBuffer);
        for (size_t index = firstTexture; index < textures.size(); ++index)
        {
            device->DestroyTexture(textures[index]);
        }
        textures.resize(firstTexture);
        device->CollectCompletedResources();
        ASSERT_EQ(results.size(), rays.size());
    }
};

TEST_P(DistanceFieldIntegrationTest, AnalyticPlaneBothSidesSlabEntryAndFiniteSegments)
{
    const DistanceFieldCheckFixture         fixture = BuildFieldCheckFixture(8u, 4.0f, {}, 2.0f);
    const HeapVector<DistanceFieldCheckRay> rays    = {
        {Vec4(0.25f, 1.25f, 1.25f, 0), Vec4(1, 0, 0, 100)},
        {Vec4(3.75f, 1.25f, 1.25f, 0), Vec4(-1, 0, 0, 100)},
        {Vec4(-2, 1.25f, 1.25f, 0), Vec4(1, 0, 0, 100)},
        {Vec4(6, 1.25f, 1.25f, 0), Vec4(-1, 0, 0, 100)},
        {Vec4(0.25f, 1.25f, 1.25f, 0), Vec4(-1, 0, 0, 100)},
        {Vec4(-2, 5, 1.25f, 0), Vec4(1, 0, 0, 100)},
        {Vec4(0.25f, 1.25f, 1.25f, 0), Vec4(1, 0, 0, 1.5f)},
        {Vec4(0.25f, 1.25f, 1.25f, 0), Vec4(glm::normalize(Vec3(1, 0.15f, 0.1f)), 100)},
        {Vec4(0.25f, 1.25f, 1.25f, 1.8f), Vec4(1, 0, 0, 100)},
    };
    HeapVector<DistanceFieldCheckResult> results;
    Trace(fixture, rays, results);
    ASSERT_EQ(results.size(), rays.size());
    for (uint32_t index = 0u; index < rays.size(); ++index)
    {
        SCOPED_TRACE(index);
        const bool hit = index < 4u || index == 7u;
        EXPECT_EQ(results[index].statistics.x, hit ? FieldHit : FieldCoveredMiss);
        if (hit)
        {
            const float expected = (2.0f - rays[index].originMinimum.x) / rays[index].directionMaximum.x;
            EXPECT_NEAR(results[index].positionDistance.w, expected, 0.01f);
            EXPECT_LT(glm::dot(Vec3(results[index].normal), Vec3(rays[index].directionMaximum)), -0.98f);
            EXPECT_GT(results[index].statistics.w, 4u);
        }
    }
}

TEST_P(DistanceFieldIntegrationTest, AnalyticBoxFacesCornersAndExternalCacheBlocker)
{
    const HeapVector<DistanceFieldCheckBox> boxes   = {{Vec3(2, 1, 1), Vec3(3, 3, 3)}};
    const DistanceFieldCheckFixture         fixture = BuildFieldCheckFixture(8u, 4.0f, boxes);
    // The receiver cache is [0,1] x [1,2] x [1,2]. Its visibility ray still hits the box at x=2.
    const HeapVector<DistanceFieldCheckRay> rays = {
        {Vec4(0.5f, 1.5f, 1.5f, 0), Vec4(1, 0, 0, 100)},
        {Vec4(3.5f, 1.5f, 1.5f, 0), Vec4(-1, 0, 0, 100)},
        {Vec4(2.5f, 0.5f, 1.5f, 0), Vec4(0, 1, 0, 100)},
        {Vec4(2.5f, 3.5f, 1.5f, 0), Vec4(0, -1, 0, 100)},
        {Vec4(2.5f, 1.5f, 0.5f, 0), Vec4(0, 0, 1, 100)},
        {Vec4(2.5f, 1.5f, 3.5f, 0), Vec4(0, 0, -1, 100)},
        {Vec4(0.5f, 0.5f, 0.5f, 0), Vec4(glm::normalize(Vec3(2, 1, 1)), 100)},
        {Vec4(0.5f, 0.5f, 0.5f, 0), Vec4(1, 0, 0, 100)},
        {Vec4(0.5f, 1.5f, 1.5f, 0), Vec4(1, 0, 0, 1.0f)},
        {Vec4(2.5f, 1.5f, 1.5f, 0), Vec4(1, 0, 0, 100)},
    };
    HeapVector<DistanceFieldCheckResult> results;
    Trace(fixture, rays, results);
    ASSERT_EQ(results.size(), rays.size());
    for (uint32_t index = 0u; index < rays.size(); ++index)
    {
        SCOPED_TRACE(index);
        const bool hit = index != 7u && index != 8u;
        EXPECT_EQ(results[index].statistics.x, hit ? FieldHit : FieldCoveredMiss);
        if (index < 6u)
        {
            EXPECT_NEAR(results[index].positionDistance.w, index == 0u ? 1.5f : 0.5f, 0.01f);
            EXPECT_LT(glm::dot(Vec3(results[index].normal), Vec3(rays[index].directionMaximum)), -0.98f);
        }
    }
    EXPECT_GT(results[0].positionDistance.x, 1.0f);
}

TEST_P(DistanceFieldIntegrationTest, UnknownRegionsAndBothIterationLimitsRemainUnavailable)
{
    DistanceFieldCheckFixture fixture       = BuildFieldCheckFixture(8u, 4.0f, {}, 2.0f);
    fixture.cells[1u + 8u * (2u + 8u * 2u)] = FieldUnknownCell;
    BuildFieldCheckHierarchy(fixture);
    const HeapVector<DistanceFieldCheckRay> rays = {
        {Vec4(0.25f, 1.25f, 1.25f, 0), Vec4(1, 0, 0, 100)},
        {Vec4(0.25f, 1.25f, 1.25f, 0), Vec4(1, 0, 0, 0.1f)},
        {Vec4(0.25f, 2.25f, 1.25f, 0), Vec4(1, 0, 0, 100)},
        {Vec4(0.25f, 2.25f, 1.25f, 0), Vec4(1, 0, 0, 100), glm::uvec4(1, 0, 0, 0)},
    };
    HeapVector<DistanceFieldCheckResult> results;
    Trace(fixture, rays, results);
    ASSERT_EQ(results.size(), rays.size());
    EXPECT_EQ(results[0].statistics.x, FieldUnavailable);
    EXPECT_EQ(results[0].normal.w, 3.0f); // Missing leaf data.
    EXPECT_EQ(results[1].statistics.x, FieldCoveredMiss);
    EXPECT_EQ(results[2].statistics.x, FieldHit);
    EXPECT_EQ(results[3].statistics.x, FieldUnavailable);
    EXPECT_EQ(results[3].statistics.z, 1u);
    EXPECT_EQ(results[3].normal.w, 2.0f); // Global work budget.

    fixture.uniform.tracing.y = 1.0f;
    Trace(fixture, {rays[2]}, results);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].statistics.x, FieldUnavailable);
    EXPECT_EQ(results[0].statistics.w, 1u);
    EXPECT_EQ(results[0].normal.w, 6.0f); // Per-brick sample budget.

    fixture = BuildFieldCheckFixture(8u, 4.0f, {});
    Trace(fixture, rays, results);
    ASSERT_EQ(results.size(), rays.size());
    for (uint32_t index = 0; index < results.size(); ++index)
    {
        const DistanceFieldCheckResult& result = results[index];
        // One map load certifies only the first leaf, not the rest of the ray's span.
        EXPECT_EQ(result.statistics.x, index == 3 ? FieldUnavailable : FieldCoveredMiss);
        EXPECT_EQ(result.statistics.w, 0u);
        EXPECT_EQ(result.normal.w, index == 3 ? 2.0f : 0.0f);
    }
}

TEST_P(DistanceFieldIntegrationTest, CancellationAtNodeFacesPreservesSuccessorCoverage)
{
    DistanceFieldCheckFixture fixture = BuildFieldCheckFixture(64u, 64.0f, {});
    fixture.uniform.dimensions.y      = 2u;
    fixture.uniform.atlasDimensions   = glm::uvec4(16, 8, 8, 0);
    fixture.atlas.resize(16u * 8u * 8u);
    std::fill(fixture.atlas.begin(), fixture.atlas.end(), uint8_t(255));
    // Every leaf references a constant positive field so traversal must cross actual leaf faces.
    std::fill(fixture.cells.begin(), fixture.cells.end(), 1u);
    BuildFieldCheckHierarchy(fixture);
    // In float32, nextafter(tExit,+inf) still reconstructs x below a crossed x=45/46
    // face for separated/fused arithmetic respectively. Mirror the query for negative travel.
    const Vec3                              origin(-982.6209106445312f, -733.8889770507812f, -588.85400390625f);
    const Vec3                              direction(0.7211251258850098f, 0.5413292050361633f, 0.4323669373989105f);
    const HeapVector<DistanceFieldCheckRay> rays = {
        {Vec4(origin, 0), Vec4(direction, 2000)},
        {Vec4(Vec3(64) - origin, 0), Vec4(-direction, 2000)},
    };
    HeapVector<DistanceFieldCheckResult> results;
    Trace(fixture, rays, results);
    ASSERT_EQ(results.size(), rays.size());
    for (const DistanceFieldCheckResult& result : results)
    {
        EXPECT_EQ(result.statistics.x, FieldCoveredMiss);
        EXPECT_EQ(result.normal.w, 0.0f);
        EXPECT_LE(result.statistics.z, fixture.uniform.dimensions.w);
    }

    const uint32_t successor[] = {46u + 64u * (38u + 64u * 27u), 17u + 64u * (25u + 64u * 36u)};
    for (uint32_t index : successor)
    {
        fixture.cells[index] = FieldUnknownCell;
    }
    BuildFieldCheckHierarchy(fixture);
    Trace(fixture, rays, results);
    ASSERT_EQ(results.size(), rays.size());
    for (const DistanceFieldCheckResult& result : results)
    {
        EXPECT_EQ(result.statistics.x, FieldUnavailable);
        EXPECT_EQ(result.normal.w, 3.0f);
    }

    // The same successor cells become blockers. A progress repair must not skip their data.
    for (uint32_t z = 0; z < 8; ++z)
    {
        for (uint32_t y = 0; y < 8; ++y)
        {
            for (uint32_t x = 8; x < 16; ++x)
            {
                fixture.atlas[x + 16u * (y + 8u * z)] = 0;
            }
        }
    }
    for (uint32_t index : successor)
    {
        fixture.cells[index] = 2u;
    }
    BuildFieldCheckHierarchy(fixture);
    Trace(fixture, rays, results);
    ASSERT_EQ(results.size(), rays.size());
    for (const DistanceFieldCheckResult& result : results)
    {
        EXPECT_EQ(result.statistics.x, FieldHit);
        EXPECT_EQ(result.statistics.y, 1u);
        EXPECT_EQ(result.normal.w, 0.0f);
    }
}

bool AnalyticFieldBoxesHit(const DistanceFieldCheckRay& ray, const HeapVector<DistanceFieldCheckBox>& boxes)
{
    bool hit = false;
    for (const DistanceFieldCheckBox& box : boxes)
    {
        float enter      = ray.originMinimum.w;
        float leave      = ray.directionMaximum.w;
        bool  intersects = true;
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            const float origin    = ray.originMinimum[axis];
            const float direction = ray.directionMaximum[axis];
            if (direction == 0.0f)
            {
                intersects = intersects && origin >= box.minimum[axis] && origin <= box.maximum[axis];
            }
            else
            {
                const float a = (box.minimum[axis] - origin) / direction;
                const float b = (box.maximum[axis] - origin) / direction;
                enter         = std::max(enter, std::min(a, b));
                leave         = std::min(leave, std::max(a, b));
            }
        }
        hit = hit || (intersects && enter < leave);
    }
    return hit;
}

TEST_P(DistanceFieldIntegrationTest, ShiftedPlaneResolutionAndToleranceExperiment)
{
    for (uint32_t side : {8u, 16u, 32u})
    {
        const float sampleSpacing = 4.0f / static_cast<float>(side * 7u);
        for (uint32_t phase = 0u; phase < 4u; ++phase)
        {
            const float               plane   = 2.0f + 0.25f * static_cast<float>(phase) * sampleSpacing;
            DistanceFieldCheckFixture fixture = BuildFieldCheckFixture(side, 4.0f, {}, plane);
            for (float tolerance : {1.0f / 255.0f, 0.125f / 7.0f, 0.25f / 7.0f, 0.5f / 7.0f})
            {
                fixture.uniform.tracing.x                    = tolerance;
                const HeapVector<DistanceFieldCheckRay> rays = {
                    {Vec4(0.5f, 2, 2, 0), Vec4(1, 0, 0, 100)},
                    {Vec4(3.5f, 2, 2, 0), Vec4(-1, 0, 0, 100)},
                    {Vec4(0.5f, 1.5f, 2, 0), Vec4(glm::normalize(Vec3(1, 0.2f, 0)), 100)},
                };
                HeapVector<DistanceFieldCheckResult> results;
                Trace(fixture, rays, results);
                ASSERT_EQ(results.size(), rays.size());
                std::string outcomes;
                for (uint32_t index = 0u; index < rays.size(); ++index)
                {
                    const DistanceFieldCheckResult& result = results[index];
                    const float expected = (plane - rays[index].originMinimum.x) / rays[index].directionMaximum.x;
                    outcomes += std::to_string(result.statistics.x) + ":" + std::to_string(result.positionDistance.w - expected)
                              + ":" + std::to_string(result.statistics.w) + " ";
                    if (tolerance == 0.5f / 7.0f)
                    {
                        EXPECT_EQ(result.statistics.x, FieldHit) << "cells=" << side << " phase=" << phase << " ray=" << index;
                        EXPECT_NEAR(result.positionDistance.w, expected, sampleSpacing * 1.1f);
                    }
                }
                RecordProperty("plane_" + std::to_string(side) + "_" + std::to_string(phase) + "_" + std::to_string(tolerance),
                               outcomes);
            }
        }
    }
}

void DistanceFieldIntegrationTest::SlitExperiment(const HeapVector<uint32_t>&   resolutions,
                                                  const HeapVector<float>&      tolerances,
                                                  const HeapVector<glm::uvec2>& phases,
                                                  bool                          recordStorage)
{
    // Ideal complete fields only: this isolates interpolation/quantization/traversal, not emission,
    // Eikonal, material coverage or lighting. Dense aperture rays use independent analytic box
    // intersections. All quality outcomes are recorded; a passing test does not qualify a preset.
    for (uint32_t side : resolutions)
    {
        const float sampleSpacing = 4.0f / static_cast<float>(side * 7u);
        for (const glm::uvec2& phase : phases)
        {
            const uint32_t                          wallPhase     = phase.x;
            const uint32_t                          aperturePhase = phase.y;
            const float                             wallMinimum = 2.0f + 0.25f * static_cast<float>(wallPhase) * sampleSpacing;
            const float                             wallMaximum = wallMinimum + 1.0f / 28.0f;
            const float                             wallCenter  = 0.5f * (wallMinimum + wallMaximum);
            const float                             aperture = 2.0f + 0.25f * static_cast<float>(aperturePhase) * sampleSpacing;
            const HeapVector<DistanceFieldCheckBox> boxes    = {
                {Vec3(wallMinimum, 0, 0), Vec3(wallMaximum, aperture - 0.025f, 4)},
                {Vec3(wallMinimum, aperture + 0.025f, 0), Vec3(wallMaximum, 4, 4)},
            };
            DistanceFieldCheckFixture fixture = BuildFieldCheckFixture(side, 4.0f, boxes);
            if (recordStorage)
            {
                const uint64_t atlasBytes      = fixture.atlas.size();
                const uint64_t mapBytes        = fixture.cells.size() * sizeof(uint32_t);
                const uint64_t hierarchyBytes  = fixture.hierarchy.size() * sizeof(uint32_t);
                uint64_t       powerOfTwoTiles = 1u;
                while (powerOfTwoTiles * powerOfTwoTiles * powerOfTwoTiles < fixture.uniform.dimensions.y)
                {
                    powerOfTwoTiles *= 2u;
                }
                const uint64_t    powerOfTwoAtlasBytes = powerOfTwoTiles * powerOfTwoTiles * powerOfTwoTiles * 512u;
                const std::string storage =
                    "bricks=" + std::to_string(fixture.uniform.dimensions.y) + " atlas_bytes=" + std::to_string(atlasBytes)
                    + " map_bytes=" + std::to_string(mapBytes) + " hierarchy_bytes=" + std::to_string(hierarchyBytes)
                    + " field_bytes=" + std::to_string(atlasBytes + mapBytes + hierarchyBytes)
                    + " power_of_two_atlas_bytes=" + std::to_string(powerOfTwoAtlasBytes);
                RecordProperty("storage_" + std::to_string(side) + "_" + std::to_string(wallPhase) + "_"
                                   + std::to_string(aperturePhase),
                               storage);
            }
            HeapVector<DistanceFieldCheckRay> rays;
            for (float angle : {0.0f, 30.0f, 60.0f})
            {
                for (float approach : {1.0f, -1.0f})
                {
                    const Vec3 direction(approach * std::cos(glm::radians(angle)), std::sin(glm::radians(angle)), 0);
                    for (int32_t offset = -4; offset <= 4; ++offset)
                    {
                        for (float bias : {0.0f, 0.002f})
                        {
                            const float originX  = approach > 0 ? 0.5f : 3.5f;
                            const float distance = (wallCenter - originX) / direction.x;
                            const Vec3  origin =
                                Vec3(originX, aperture + static_cast<float>(offset) * 0.01f - distance * direction.y, 2)
                                + direction * bias;
                            rays.push_back({Vec4(origin, 0), Vec4(direction, 100)});
                        }
                    }
                }
            }
            for (float tolerance : tolerances)
            {
                fixture.uniform.tracing.x = tolerance;
                HeapVector<DistanceFieldCheckResult> results;
                Trace(fixture, rays, results);
                ASSERT_EQ(results.size(), rays.size());
                uint32_t open        = 0u;
                uint32_t blocked     = 0u;
                uint32_t leaks       = 0u;
                uint32_t overblocks  = 0u;
                uint32_t unavailable = 0u;
                uint32_t samples     = 0u;
                for (uint32_t index = 0u; index < rays.size(); ++index)
                {
                    const bool     expectedHit  = AnalyticFieldBoxesHit(rays[index], boxes);
                    const uint32_t status       = results[index].statistics.x;
                    blocked                    += expectedHit ? 1u : 0u;
                    open                       += expectedHit ? 0u : 1u;
                    leaks                      += expectedHit && status == FieldCoveredMiss ? 1u : 0u;
                    overblocks                 += !expectedHit && status == FieldHit ? 1u : 0u;
                    unavailable                += status == FieldUnavailable ? 1u : 0u;
                    samples                    += results[index].statistics.w;
                    EXPECT_LE(status, FieldUnavailable);
                    EXPECT_LE(results[index].statistics.z, fixture.uniform.dimensions.w);
                }
                const std::string key      = "slit_" + std::to_string(side) + "_" + std::to_string(wallPhase) + "_"
                                           + std::to_string(aperturePhase) + "_" + std::to_string(tolerance);
                const std::string outcomes = "open=" + std::to_string(open) + " blocked=" + std::to_string(blocked)
                                           + " leaks=" + std::to_string(leaks) + " overblocks=" + std::to_string(overblocks)
                                           + " unavailable=" + std::to_string(unavailable)
                                           + " samples=" + std::to_string(samples);
                RecordProperty(key, outcomes);
            }
        }
    }
}

TEST_P(DistanceFieldIntegrationTest, ThinWallSlitResolutionTolerancePhaseAndBiasExperiment)
{
    HeapVector<glm::uvec2> phases;
    for (uint32_t wall = 0u; wall < 4u; ++wall)
    {
        for (uint32_t aperture = 0u; aperture < 4u; ++aperture)
        {
            phases.push_back(glm::uvec2(wall, aperture));
        }
    }
    SlitExperiment({8u, 16u, 32u}, {1.0f / 255.0f, 0.125f / 7.0f, 0.25f / 7.0f, 0.5f / 7.0f}, phases, false);
}

TEST_P(DistanceFieldIntegrationTest, FinerLocalSlitExperiment)
{
    // Four crossed phase pairs bound runtime; they are a subset of the coarse 16-pair experiment.
    // Atlas/map/pyramid bytes describe this ideal fixture, excluding builder scratch and retirement.
    SlitExperiment({64u, 128u}, {0.125f / 7.0f, 0.25f / 7.0f},
                   {glm::uvec2(0, 0), glm::uvec2(1, 2), glm::uvec2(2, 1), glm::uvec2(3, 3)}, true);
}

INSTANTIATE_TEST_SUITE_P(SubmissionModes, DistanceFieldIntegrationTest, testing::Values(0u, 1u, 2u, 3u));
