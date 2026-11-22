// Shares native device setup and resource teardown with the array texture tests.
#include "NormalSamplingFixtures.inl"

struct NormalSamplingQuery
{
    Vec4       uvLodScale;
    Vec4       gradients;
    glm::uvec4 bindings;
};
static_assert(sizeof(NormalSamplingQuery) == 48);

struct NormalSamplingGroup
{
    uint32_t first, count, fixture;
};

void DecodeNormalFixture(const char* text, HeapVector<uint8_t>& bytes)
{
    const size_t length = std::strlen(text);
    ASSERT_EQ(length % 2, 0u);
    bytes.resize(length / 2);
    for (size_t index = 0; index < length; ++index)
    {
        const char digit = text[index];
        ASSERT_TRUE((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f'));
        const uint8_t value = uint8_t(digit <= '9' ? digit - '0' : digit - 'a' + 10);
        bytes[index / 2]    = uint8_t((bytes[index / 2] << 4) | value);
    }
}

void BuildNormalSamplingScene(sg::Scene& scene)
{
    for (const NormalSamplingFixture& fixture : NormalSamplingFixtures)
    {
        for (uint32_t variant = 0; variant < 2; ++variant)
        {
            SharedPtr<asset::ImagePixels, MultiThreadCounter> pixels = MakeShared<asset::ImagePixels, MultiThreadCounter>();
            pixels->width                                            = fixture.width;
            pixels->height                                           = fixture.height;
            pixels->format = variant == 0 ? asset::Format::R8G8B8A8_UNORM : fixture.format;
            ASSERT_LE(fixture.mipCount, 5u);
            for (uint32_t level = 0; level < fixture.mipCount; ++level)
            {
                pixels->mips.emplace_back();
                ASSERT_NO_FATAL_FAILURE(
                    DecodeNormalFixture(variant == 0 ? fixture.original[level] : fixture.cooked[level], pixels->mips.back()));
            }
            pixels->bytes                  = pixels->mips[0];
            UniquePtr<sg::Texture> texture = MakeUnique<sg::Texture>(fixture.name);
            texture->width                 = pixels->width;
            texture->height                = pixels->height;
            texture->format                = pixels->format;
            texture->pixels                = pixels;
            texture->interpretation        = pixels->format == asset::Format::BC5_UNORM_BLOCK
                                               ? asset::TextureInterpretation::NormalXYPositiveZ
                                               : asset::TextureInterpretation::Default;
            scene.AddComponent(std::move(texture));
        }
    }
}

double SampledNormalAngle(const Vec4& a, const Vec4& b)
{
    double aa = 0, bb = 0, ab = 0;
    for (uint32_t channel = 0; channel < 3; ++channel)
    {
        aa += double(a[channel]) * a[channel];
        bb += double(b[channel]) * b[channel];
        ab += double(a[channel]) * b[channel];
    }
    return std::acos(std::clamp(ab / std::sqrt(aa * bb), -1.0, 1.0)) * 180 / 3.14159265358979323846;
}

void BuildNormalSamplingQueries(bool                             gradients,
                                bool                             regression,
                                HeapVector<NormalSamplingQuery>& queries,
                                HeapVector<NormalSamplingGroup>& groups)
{
    for (uint32_t fixtureIndex = 0; fixtureIndex < std::size(NormalSamplingFixtures); ++fixtureIndex)
    {
        const NormalSamplingFixture& fixture  = NormalSamplingFixtures[fixtureIndex];
        const bool                   selected = regression ? fixtureIndex >= 10 : !fixture.legacy;
        if (selected)
        {
            for (uint32_t sampler = 0; sampler < (gradients ? 8u : 16u); ++sampler)
            {
                for (uint32_t operation = 0; operation < (gradients ? 12u : fixture.mipCount * 4 + 1); ++operation)
                {
                    groups.push_back({static_cast<uint32_t>(queries.size()), 17 * 17, fixtureIndex});
                    const float angle = float(operation % 3) * 3.14159265358979323846f / 4;
                    const float major = std::exp2(float(operation / 3 + 1));
                    const Vec4  footprint(std::cos(angle) * major / fixture.width, std::sin(angle) * major / fixture.height,
                                          -std::sin(angle) * major / (16 * fixture.width),
                                          std::cos(angle) * major / (16 * fixture.height));
                    for (uint32_t y = 0; y < 17; ++y)
                    {
                        for (uint32_t x = 0; x < 17; ++x)
                        {
                            NormalSamplingQuery query;
                            // Non-dyadic coordinates include both borders and repeat seams.
                            query.uvLodScale = Vec4((float(x) - 2.137f) / 13, (float(y) - 2.371f) / 13,
                                                    gradients ? -1.0f : float(operation) / 4, 1);
                            query.gradients  = footprint;
                            query.bindings = glm::uvec4(fixtureIndex * 2, fixtureIndex * 2 + 1,
                                                        fixture.format == asset::Format::BC5_UNORM_BLOCK ? 1 : 0, sampler + 1);
                            queries.push_back(query);
                        }
                    }
                }
            }
        }
    }
}

class NormalSceneTextureIntegrationTest : public ArraySceneTextureIntegrationTest
{
protected:
    void CreateNormalSamplers(bool gradients, HeapVector<RHISampler*>& samplers)
    {
        for (uint32_t index = 0; index < (gradients ? 8u : 16u); ++index)
        {
            RHISamplerCreateInfo info  = RHISamplerCreateInfo::CreateLinearRepeat();
            const uint32_t       wraps = gradients ? index >> 1 : index >> 2;
            info.repeatU               = (wraps & 1) != 0 ? RHISamplerRepeatMode::eRepeat : RHISamplerRepeatMode::eClampToEdge;
            info.repeatV               = (wraps & 2) != 0 ? RHISamplerRepeatMode::eRepeat : RHISamplerRepeatMode::eClampToEdge;
            info.useAnisotropy         = gradients && (index & 1) != 0;
            info.maxAnisotropy         = info.useAnisotropy ? 16.0f : 1.0f;
            if (!gradients)
            {
                info.minFilter = info.magFilter = (index & 1) != 0 ? RHISamplerFilter::eLinear : RHISamplerFilter::eNearest;
                info.mipFilter                  = (index & 2) != 0 ? RHISamplerFilter::eLinear : RHISamplerFilter::eNearest;
            }
            RHISampler* sampler = device->CreateSampler(info);
            ASSERT_NE(sampler, nullptr);
            samplers.push_back(sampler);
        }
    }

    void CheckNormalDirections(const HeapVector<Vec4>& values, const HeapVector<NormalSamplingGroup>& groups)
    {
        double   worstRms[std::size(NormalSamplingFixtures)]{};
        double   maximum[std::size(NormalSamplingFixtures)]{};
        uint32_t counts[std::size(NormalSamplingFixtures)]{};
        for (const NormalSamplingGroup& group : groups)
        {
            double sumSquares = 0;
            for (uint32_t sample = group.first; sample < group.first + group.count; ++sample)
            {
                const Vec4& original = values[sample * 2];
                const Vec4& cooked   = values[sample * 2 + 1];
                for (uint32_t channel = 0; channel < 3; ++channel)
                {
                    ASSERT_TRUE(std::isfinite(original[channel]) && std::isfinite(cooked[channel]));
                }
                const double error = SampledNormalAngle(original, cooked);
                ASSERT_TRUE(std::isfinite(error));
                sumSquares             += error * error;
                maximum[group.fixture]  = std::max(maximum[group.fixture], error);
            }
            counts[group.fixture]   += group.count;
            worstRms[group.fixture]  = std::max(worstRms[group.fixture], std::sqrt(sumSquares / group.count));
        }
        for (uint32_t index = 0; index < std::size(NormalSamplingFixtures); ++index)
        {
            if (counts[index] != 0)
            {
                const NormalSamplingFixture& fixture = NormalSamplingFixtures[index];
                SCOPED_TRACE(fixture.name);
                RecordProperty(std::string(fixture.name) + "_samples", counts[index]);
                RecordProperty(std::string(fixture.name) + "_rms_degrees", std::to_string(worstRms[index]));
                RecordProperty(std::string(fixture.name) + "_max_degrees", std::to_string(maximum[index]));
                if (fixture.legacy)
                {
                    // Negative control: the real GPU must expose the old acceptance bug.
                    EXPECT_NEAR(maximum[index], 10.475082330, 0.05);
                }
                else if (fixture.format == asset::Format::R8G8B8A8_UNORM)
                {
                    EXPECT_LT(maximum[index], 0.00001);
                }
                else
                {
                    EXPECT_LE(worstRms[index], 1.0);
                    EXPECT_LE(maximum[index], 5.0);
                }
            }
        }
    }

    void SampleNormals(bool gradients, bool regression)
    {
        RecordProperty("device_name", properties.deviceName);
        RecordProperty("vendor_id", std::to_string(properties.vendorID));
        RecordProperty("driver_version", std::to_string(properties.driverVersion));
        RecordProperty("subtexel_precision_bits", properties.limits.subTexelPrecisionBits);
        RecordProperty("mipmap_precision_bits", properties.limits.mipmapPrecisionBits);
        ASSERT_TRUE(device->PrepareForSceneReplacement());
        sg::Scene source;
        ASSERT_NO_FATAL_FAILURE(BuildNormalSamplingScene(source));
        HeapVector<RHITextureView*> views;
        device->LoadSceneTextures(&source, views);
        ASSERT_EQ(views.size(), std::size(NormalSamplingFixtures) * 2);
        for (uint32_t index = 0; index < views.size(); ++index)
        {
            RHITextureView* view = views[index];
            ASSERT_NE(view, nullptr);
            const asset::Format expected =
                index % 2 == 0 ? asset::Format::R8G8B8A8_UNORM : NormalSamplingFixtures[index / 2].format;
            EXPECT_EQ(view->GetFormat(), static_cast<DataFormat>(expected));
            EXPECT_EQ(view->GetTexture()->GetFormat(), static_cast<DataFormat>(expected));
            EXPECT_EQ(view->GetTexture()->GetBaseInfo().mipmaps, NormalSamplingFixtures[index / 2].mipCount);
            if (std::find(images.begin(), images.end(), view->GetTexture()) == images.end())
            {
                images.push_back(view->GetTexture());
            }
        }
        HeapVector<RHISampler*> samplers;
        ASSERT_NO_FATAL_FAILURE(CreateNormalSamplers(gradients, samplers));
        const std::string shader =
            std::filesystem::relative(std::filesystem::path(RDG_REFLECTION_TEST_PATH) / "scene_normal_sampling.comp.spv",
                                      SPV_SHADER_PATH)
                .generic_string();
        ShaderProgram* program = ZEN_NEW() ComputeFileSP(device, "NormalSamplingSP", shader);
        ShaderProgramManager::GetInstance().StoreProgram(program);
        ASSERT_NE(program->GetShader(), nullptr);
        HeapVector<NormalSamplingQuery> queries;
        HeapVector<NormalSamplingGroup> groups;
        BuildNormalSamplingQueries(gradients, regression, queries, groups);
        const uint32_t count = static_cast<uint32_t>(queries.size());
        ASSERT_GT(count, 0u);
        RHIBuffer* input = device->CreateStorageBuffer(count * sizeof(NormalSamplingQuery),
                                                       reinterpret_cast<const uint8_t*>(queries.data()), "normal_queries");
        buffers.push_back(input);
        RHIBuffer* output   = Buffer(false, count * 2);
        RHIBuffer* readback = Buffer(true, count * 2);
        ASSERT_NE(input, nullptr);
        ASSERT_NE(output, nullptr);
        ASSERT_NE(readback, nullptr);
        RenderGraph& graph = *device->GetCurrentFrameRDG();
        ASSERT_TRUE(graph.Begin());
        RDGComputePassDesc pass;
        pass.SetShaderProgramName("NormalSamplingSP");
        pass.BindStorageBuffer("Results", output, RDGContentGuarantee::eFullWrite);
        pass.BindStorageBuffer("Queries", input);
        BindSceneTextureArray(pass, samplers[0], views, samplers);
        graph.AddComputePass(std::move(pass)).RecordPassCommands([count](RDGPassCmdEncoder& encoder) {
            encoder.Dispatch((count + 63) / 64, 1, 1);
        });
        graph.AddTransferPass("normal_sampling_readback").CopyBuffer(output, readback, {0, 0, count * 2 * sizeof(Vec4)});
        ASSERT_TRUE(graph.End()) << graph.GetResult().message;
        ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
        device->FlushRHIThread();
        device->WaitForIdle();
        const uint8_t* mapped = readback->Map();
        ASSERT_NE(mapped, nullptr);
        HeapVector<Vec4> values(count * 2);
        std::memcpy(values.data(), mapped, values.size() * sizeof(Vec4));
        readback->Unmap();
        if (gradients)
        {
            uint32_t changed = 0;
            for (uint32_t index = 0; index < count; ++index)
            {
                if (queries[index].bindings.w % 2 == 1)
                {
                    // Neighboring sampler blocks differ only in anisotropy enablement.
                    const uint32_t paired = index + 12 * 17 * 17;
                    ASSERT_LT(paired, count);
                    ASSERT_EQ(queries[paired].bindings.w, queries[index].bindings.w + 1);
                    changed += glm::length(values[index * 2] - values[paired * 2]) > 0.00001f ? 1u : 0u;
                }
            }
            EXPECT_GT(changed, 0u);
            RecordProperty("anisotropy_changed_reference_samples", changed);
        }
        ASSERT_NO_FATAL_FAILURE(CheckNormalDirections(values, groups));
        RecordProperty("sample_pairs", count);
        RecordProperty("independent_rms_groups", static_cast<uint32_t>(groups.size()));
        graph.Reset();
    }
};

TEST_P(NormalSceneTextureIntegrationTest, CookedNormalsRespectLimitsAcrossTexelAndMipFilters)
{
    SampleNormals(false, false);
}

TEST_P(NormalSceneTextureIntegrationTest, CookedNormalsRespectLimitsAcrossAnisotropicFootprints)
{
    if (!device->GetGPUInfo().supportSamplerAnisotropy)
    {
        GTEST_SKIP() << "Sampler anisotropy is unsupported on this device";
    }
    const float limit = properties.limits.maxSamplerAnisotropy;
    RecordProperty("effective_anisotropy", std::to_string(std::min(16.0f, limit)));
    SampleNormals(true, false);
}

TEST_P(NormalSceneTextureIntegrationTest, MipRejectionRetainsDirectionsAndLegacyControlExceedsLimits)
{
    SampleNormals(false, true);
}

INSTANTIATE_TEST_SUITE_P(Execution,
                         NormalSceneTextureIntegrationTest,
                         testing::Values(RHIExecutionMode::eInline, RHIExecutionMode::eThreaded));
