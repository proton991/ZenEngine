#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RenderCore/V2/Renderer/RendererUtils.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"
#include <volk.h>
#include "ZenRHI/Vulkan/VulkanInterop.h"
#include "SceneGraph/Scene.h"
#include "SpatialQualificationFailure.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <filesystem>
#include <cmath>
#include <chrono>
#include <cstring>

namespace
{
using namespace zen;
using namespace zen::rc;
using zen::test::SpatialQualificationPipelineFailure;

// Two layers and two view formats, followed by their standalone counterparts
// and a duplicate logical binding. No processor dependency enters this target.
void BuildArrayAndReferenceTextures(sg::Scene& scene, DataFormat format)
{
    const uint8_t red[]   = {0x40, 0x32, 0x99, 0x42, 0x51, 0x28, 0x80, 0x40, 0, 0, 0, 0, 0, 0, 0, 0};
    const uint8_t green[] = {0x40, 0x85, 0x42, 0xab, 0xf5, 0x78, 0x40, 0x20, 0, 0, 0, 0, 0, 0, 0, 0};
    SharedPtr<asset::ImagePixels, MultiThreadCounter> layers[2];
    for (uint32_t layer = 0; layer < 2; ++layer)
    {
        layers[layer]              = MakeShared<asset::ImagePixels, MultiThreadCounter>();
        asset::ImagePixels& pixels = *layers[layer];
        pixels.width               = 5;
        pixels.height              = 7;
        pixels.format              = static_cast<asset::Format>(format);
        for (uint32_t level = 0; level < 3; ++level)
        {
            uint64_t bytes = 0;
            ASSERT_TRUE(GetTextureFormatLevelSize(format, std::max(1u, 5u >> level), std::max(1u, 7u >> level), 1, bytes));
            pixels.mips.emplace_back(static_cast<size_t>(bytes), 0);
            HeapVector<uint8_t>& mip    = pixels.mips.back();
            const uint32_t       stride = GetTextureFormatBlockInfo(format).bytes;
            for (size_t offset = 0; offset < mip.size(); offset += stride)
            {
                if (format == DataFormat::eBC7UNORMBlock)
                {
                    std::memcpy(mip.data() + offset, (layer + level) % 2 == 0 ? red : green, 16);
                }
                else if (format == DataFormat::eR8G8B8A8UNORM)
                {
                    mip[offset]          = uint8_t(40 + layer * 120 + level * 5);
                    mip[offset + 1]      = uint8_t(180 - layer * 120);
                    mip[offset + 2]      = uint8_t(20 + level * 30);
                    const uint32_t pixel = static_cast<uint32_t>(offset / stride);
                    const uint32_t width = std::max(1u, 5u >> level);
                    mip[offset + 3]      = (pixel % width + pixel / width + layer) % 2 == 0 ? 20 : 230;
                }
                else
                {
                    mip[offset] = mip[offset + 1] = uint8_t(80 + layer * 80 + level * 4);
                    if (format == DataFormat::eBC5UNORMBlock)
                    {
                        mip[offset + 8] = mip[offset + 9] = uint8_t(100 + layer * 40);
                    }
                }
            }
        }
        pixels.bytes = pixels.mips[0];
    }
    SharedPtr<asset::ImagePixels, MultiThreadCounter> page = MakeShared<asset::ImagePixels, MultiThreadCounter>();
    page->width                                            = 5;
    page->height                                           = 7;
    page->format                                           = static_cast<asset::Format>(format);
    page->arrayLayers                                      = 2;
    for (uint32_t level = 0; level < 3; ++level)
    {
        page->mips.push_back(layers[0]->mips[level]);
        page->mips.back().push_back(layers[1]->mips[level]);
    }
    page->bytes = page->mips[0];
    for (uint32_t index = 0; index < 9; ++index)
    {
        const uint32_t         logical = index % 4;
        const uint32_t         layer   = logical / 2;
        UniquePtr<sg::Texture> texture = MakeUnique<sg::Texture>("array_native_sample");
        texture->width                 = 5;
        texture->height                = 7;
        texture->pixels                = index < 4 || index == 8 ? page : layers[layer];
        texture->arrayLayer            = index < 4 || index == 8 ? layer : 0;
        texture->format                = static_cast<asset::Format>(format);
        if (logical % 2 != 0 && (format == DataFormat::eR8G8B8A8UNORM || format == DataFormat::eBC7UNORMBlock))
        {
            texture->format =
                format == DataFormat::eR8G8B8A8UNORM ? asset::Format::R8G8B8A8_SRGB : asset::Format::BC7_SRGB_BLOCK;
        }
        texture->interpretation = format == DataFormat::eBC5UNORMBlock ? asset::TextureInterpretation::NormalXYPositiveZ
                                                                       : asset::TextureInterpretation::Default;
        scene.AddComponent(std::move(texture));
    }
}

void BuildSpatialAndReferenceTextures(sg::Scene& scene)
{
    UniquePtr<sg::Sampler> sampler = MakeUnique<sg::Sampler>("spatial_linear_clamp");
    sampler->minFilter = sampler->magFilter = sampler->mipFilter = asset::TextureFilter::Linear;
    sampler->wrapS = sampler->wrapT = asset::SamplerAddressMode::ClampToEdge;
    scene.AddComponent(std::move(sampler));
    SharedPtr<asset::ImagePixels, MultiThreadCounter> tiles[2];
    SharedPtr<asset::ImagePixels, MultiThreadCounter> page = MakeShared<asset::ImagePixels, MultiThreadCounter>();
    page->width                                            = 32;
    page->height                                           = 16;
    page->format                                           = asset::Format::R8G8B8A8_UNORM;
    page->bytes                                            = HeapVector<uint8_t>(32 * 16 * 4, 255);
    const asset::ImageRegion regions[]                     = {{4, 4, 8, 4}, {20, 4, 4, 8}};
    for (uint32_t tile = 0; tile < 2; ++tile)
    {
        tiles[tile]                      = MakeShared<asset::ImagePixels, MultiThreadCounter>();
        asset::ImagePixels&       pixels = *tiles[tile];
        const asset::ImageRegion& region = regions[tile];
        pixels.width                     = region.width;
        pixels.height                    = region.height;
        pixels.format                    = page->format;
        pixels.bytes.resize(pixels.width * pixels.height * 4);
        for (uint32_t y = 0; y < pixels.height; ++y)
        {
            for (uint32_t x = 0; x < pixels.width; ++x)
            {
                const size_t offset      = (y * pixels.width + x) * 4;
                pixels.bytes[offset]     = uint8_t(x * 31 + y * 17 + tile * 33);
                pixels.bytes[offset + 1] = uint8_t(y * 53 + x * 7 + tile * 19);
                pixels.bytes[offset + 2] = uint8_t(x * 11 + y * 29 + tile * 71);
                pixels.bytes[offset + 3] = (x + y + tile) % 2 == 0 ? 0 : 255;
            }
        }
        pixels.mips.push_back(pixels.bytes);
        for (int32_t y = -4; y < static_cast<int32_t>(region.height) + 4; ++y)
        {
            for (int32_t x = -4; x < static_cast<int32_t>(region.width) + 4; ++x)
            {
                const uint32_t sx  = static_cast<uint32_t>(std::clamp(x, 0, static_cast<int32_t>(region.width) - 1));
                const uint32_t sy  = static_cast<uint32_t>(std::clamp(y, 0, static_cast<int32_t>(region.height) - 1));
                const size_t   dst = ((region.y + y) * page->width + region.x + x) * 4;
                std::memcpy(page->bytes.data() + dst, pixels.bytes.data() + (sy * region.width + sx) * 4, 4);
            }
        }
    }
    page->mips.push_back(page->bytes);
    for (uint32_t index = 0; index < 9; ++index)
    {
        const uint32_t         logical = index % 4;
        const uint32_t         tile    = logical / 2;
        const bool             packed  = index < 4 || index == 8;
        UniquePtr<sg::Texture> texture = MakeUnique<sg::Texture>("spatial_native_sample");
        texture->pixels                = packed ? page : tiles[tile];
        texture->width                 = texture->pixels->width;
        texture->height                = texture->pixels->height;
        texture->format                = logical % 2 == 0 ? asset::Format::R8G8B8A8_UNORM : asset::Format::R8G8B8A8_SRGB;
        texture->atlasRegion           = packed ? regions[tile] : asset::ImageRegion{};
        texture->index                 = index;
        texture->samplerIndex          = 0;
        scene.AddComponent(std::move(texture));
    }
}

class ArraySceneTextureIntegrationTest : public testing::TestWithParam<RHIExecutionMode>
{
protected:
    static VKAPI_ATTR VkBool32 VKAPI_CALL Validation(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                                     VkDebugUtilsMessageTypeFlagsEXT,
                                                     const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                     void*)
    {
        ADD_FAILURE() << data->pMessage;
        return VK_FALSE;
    }

    void SetUp() override
    {
        zen::rc::RenderConfig::GetInstance().rhiDevice.SetRayTracingDisabled(true);
        device = ZEN_NEW() RenderDevice(RHIAPIType::eVulkan, 2, GetParam(), AsyncComputeMode::eDisabled);
        device->Init(nullptr);
        vkGetPhysicalDeviceProperties(GetVulkanDeviceHandles(*GDynamicRHI).physicalDevice, &properties);
        VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        info.pfnUserCallback = Validation;
        ASSERT_EQ(vkCreateDebugUtilsMessengerEXT(GetVulkanDeviceHandles(*GDynamicRHI).instance, &info, nullptr, &messenger),
                  VK_SUCCESS);
        const std::string shader =
            std::filesystem::relative(std::filesystem::path(RDG_REFLECTION_TEST_PATH) / "scene_array_views.comp.spv",
                                      SPV_SHADER_PATH)
                .generic_string();
        ShaderProgram* program = ZEN_NEW() ComputeFileSP(device, "ArrayLayerSamplingSP", shader);
        ShaderProgramManager::GetInstance().StoreProgram(program);
        ASSERT_NE(program->GetShader(), nullptr);
    }

    void TearDown() override
    {
        device->FlushRHIThread();
        device->WaitForIdle();
        for (RHITexture* image : images)
        {
            device->ReleaseSceneTexture(image);
        }
        for (RHIBuffer* buffer : buffers)
        {
            device->DestroyBuffer(buffer);
        }
        ShaderProgramManager::GetInstance().Destroy();
        vkDestroyDebugUtilsMessengerEXT(GetVulkanDeviceHandles(*GDynamicRHI).instance, messenger, nullptr);
        device->Destroy();
        ZEN_DELETE(device);
        zen::rc::RenderConfig::GetInstance().rhiDevice.SetRayTracingDisabled(false);
    }

    RHIBuffer* Buffer(bool readback, uint32_t count = 162)
    {
        RHIBufferCreateInfo info;
        info.size         = count * sizeof(Vec4);
        info.allocateType = readback ? RHIBufferAllocateType::eCPURead : RHIBufferAllocateType::eGPU;
        if (readback)
        {
            info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);
        }
        else
        {
            info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer);
        }
        RHIBuffer* buffer = device->CreateBuffer(info);
        buffers.push_back(buffer);
        return buffer;
    }

    static void Dispatch(RDGPassCmdEncoder& encoder)
    {
        encoder.Dispatch(162, 1, 1);
    }

    void Compare(DataFormat format, bool anisotropic = false)
    {
        // Each case replaces the scene's fixed bindless slots through the same
        // completion/heap-reset boundary used by the editor.
        ASSERT_TRUE(device->PrepareForSceneReplacement());
        sg::Scene source;
        BuildArrayAndReferenceTextures(source, format);
        HeapVector<RHITextureView*> views;
        device->LoadSceneTextures(&source, views);
        ASSERT_EQ(views.size(), 9u);
        for (RHITextureView* view : views)
        {
            ASSERT_NE(view, nullptr);
            if (std::find(images.begin(), images.end(), view->GetTexture()) == images.end())
            {
                images.push_back(view->GetTexture());
            }
        }
        EXPECT_EQ(views[0], views[8]);
        for (uint32_t binding = 0; binding < 4; ++binding)
        {
            EXPECT_EQ(views[binding]->GetTexture(), views[0]->GetTexture());
            EXPECT_EQ(views[binding]->GetSubResourceRange().baseArrayLayer, binding / 2);
            EXPECT_EQ(views[binding]->GetSubResourceRange().layerCount, 1u);
            EXPECT_NE(views[binding]->GetTexture(), views[binding + 4]->GetTexture());
        }
        RHIBuffer* output   = Buffer(false);
        RHIBuffer* readback = Buffer(true);
        ASSERT_NE(output, nullptr);
        ASSERT_NE(readback, nullptr);
        RHISamplerCreateInfo sampler = RHISamplerCreateInfo::CreateLinearRepeat();
        sampler.repeatU = sampler.repeatV = sampler.repeatW = RHISamplerRepeatMode::eClampToEdge;
        sampler.useAnisotropy                               = anisotropic;
        sampler.maxAnisotropy                               = anisotropic ? 16.0f : 1.0f;
        RenderGraph& graph                                  = *device->GetCurrentFrameRDG();
        ASSERT_TRUE(graph.Begin());
        RDGComputePassDesc pass;
        pass.SetShaderProgramName("ArrayLayerSamplingSP");
        pass.BindStorageBuffer("Results", output, RDGContentGuarantee::eFullWrite);
        RHISampler* sampling = device->CreateSampler(sampler);
        BindSceneTextureArray(pass, sampling, views, {sampling});
        graph.AddComputePass(std::move(pass)).RecordPassCommands(Dispatch);
        graph.AddTransferPass("array_samples_readback").CopyBuffer(output, readback, {0, 0, 162 * sizeof(Vec4)});
        ASSERT_TRUE(graph.End()) << graph.GetResult().message;
        ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
        device->FlushRHIThread();
        device->WaitForIdle();
        const uint8_t* mapped = readback->Map();
        ASSERT_NE(mapped, nullptr);
        Vec4 values[162];
        std::memcpy(values, mapped, sizeof(values));
        readback->Unmap();
        for (uint32_t sample = 0; sample < 72; ++sample)
        {
            for (uint32_t channel = 0; channel < 4; ++channel)
            {
                ASSERT_TRUE(std::isfinite(values[sample][channel]));
                EXPECT_EQ(values[sample][channel], values[sample + 72][channel]);
            }
        }
        EXPECT_NE(values[0].x, values[36].x); // Distinct layers must actually be sampled.
        for (uint32_t sample = 0; sample < 18; ++sample)
        {
            EXPECT_EQ(values[sample], values[144 + sample]);
        }
        graph.Reset();
    }

    RenderDevice*              device{nullptr};
    VkPhysicalDeviceProperties properties{};
    VkDebugUtilsMessengerEXT   messenger{VK_NULL_HANDLE};
    HeapVector<RHITexture*>    images;
    HeapVector<RHIBuffer*>     buffers;
};

TEST_P(ArraySceneTextureIntegrationTest, PackedLayerViewsMatchStandaloneSamplingAcrossFormatsAndMips)
{
    for (DataFormat format :
         {DataFormat::eR8G8B8A8UNORM, DataFormat::eBC7UNORMBlock, DataFormat::eBC4UNORMBlock, DataFormat::eBC5UNORMBlock})
    {
        SCOPED_TRACE(static_cast<uint32_t>(format));
        Compare(format);
    }
}

TEST_P(ArraySceneTextureIntegrationTest, PackedLayerViewsMatchStandaloneAnisotropicGradients)
{
    if (!device->GetGPUInfo().supportSamplerAnisotropy)
    {
        GTEST_SKIP() << "Sampler anisotropy is unsupported on this device";
    }
    const float limit = properties.limits.maxSamplerAnisotropy;
    RecordProperty("effective_anisotropy", std::to_string(std::min(16.0f, limit)));
    for (DataFormat format :
         {DataFormat::eR8G8B8A8UNORM, DataFormat::eBC7UNORMBlock, DataFormat::eBC4UNORMBlock, DataFormat::eBC5UNORMBlock})
    {
        SCOPED_TRACE(static_cast<uint32_t>(format));
        Compare(format, true);
    }
}

TEST_P(ArraySceneTextureIntegrationTest, SpatialMaterialBindingsMatchStandaloneAndRayAlphaSampling)
{
    ASSERT_EQ(device->GetGPUInfo().subTexelPrecisionBits, properties.limits.subTexelPrecisionBits);
    RecordProperty("subtexel_precision_bits", device->GetGPUInfo().subTexelPrecisionBits);
    ASSERT_TRUE(device->PrepareForSceneReplacement());
    const std::chrono::steady_clock::time_point qualificationBegin = std::chrono::steady_clock::now();
    ASSERT_TRUE(device->QualifySpatialTextureSampling());
    const double qualificationMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - qualificationBegin).count();
    RecordProperty("qualification_ms", std::to_string(qualificationMs));
    RecordProperty("device_name", device->GetGPUInfo().deviceName.data());
    RecordProperty("rounding_rule", static_cast<int>(device->GetTextureSamplingParameters().y));
    const RHICompletionSet qualifiedSerials = device->GetSubmittedSerials();
    const Vec4             qualified        = device->GetTextureSamplingParameters();
    ASSERT_TRUE(device->QualifySpatialTextureSampling());
    EXPECT_EQ(device->GetTextureSamplingParameters(), qualified);
    EXPECT_EQ(device->GetSubmittedSerials().Get(RHICommandContextType::eGraphics),
              qualifiedSerials.Get(RHICommandContextType::eGraphics));
    EXPECT_EQ(device->GetSubmittedSerials().Get(RHICommandContextType::eTransfer),
              qualifiedSerials.Get(RHICommandContextType::eTransfer));
    sg::Scene source;
    BuildSpatialAndReferenceTextures(source);
    HeapVector<RHITextureView*> views;
    device->LoadSceneTextures(&source, views);
    ASSERT_EQ(views.size(), 9u);
    for (RHITextureView* view : views)
    {
        ASSERT_NE(view, nullptr);
        if (std::find(images.begin(), images.end(), view->GetTexture()) == images.end())
        {
            images.push_back(view->GetTexture());
        }
    }
    EXPECT_EQ(views[0], views[2]);
    EXPECT_EQ(views[0], views[8]);
    EXPECT_EQ(views[1], views[3]);
    EXPECT_EQ(views[0]->GetTexture(), views[1]->GetTexture());
    HeapVector<sg::MaterialData> materialData;
    for (sg::Texture* texture : source.GetComponents<sg::Texture>())
    {
        sg::Material material("spatial_sample");
        material.m_pBaseColorTexture                       = texture;
        material.m_pMetallicRoughnessTexture               = texture;
        material.m_pNormalTexture                          = texture;
        material.m_pOcclusionTexture                       = texture;
        material.m_pEmissiveTexture                        = texture;
        material.texCoordSets.baseColor                    = 0;
        material.textureTransforms[0].row0                 = Vec4(0.75f, 0.125f, 0.0625f, 1);
        material.textureTransforms[0].row1                 = Vec4(-0.125f, 0.75f, 0.1875f, 0);
        material.features.clearcoatNormalTexture.texture   = texture;
        material.features.clearcoatNormalTexture.texCoord  = 1;
        material.features.clearcoatNormalTexture.transform = material.textureTransforms[0];
        material.SetData();
        material.data.textureSampling = device->GetTextureSamplingParameters();
        materialData.push_back(material.data);
    }
    const std::string shader =
        std::filesystem::relative(std::filesystem::path(RDG_REFLECTION_TEST_PATH) / "scene_spatial_views.comp.spv",
                                  SPV_SHADER_PATH)
            .generic_string();
    ShaderProgram* program = ZEN_NEW() ComputeFileSP(device, "SpatialMaterialSamplingSP", shader);
    ShaderProgramManager::GetInstance().StoreProgram(program);
    ASSERT_NE(program->GetShader(), nullptr);
    constexpr uint32_t perBinding = 129 * 129 * 3;
    constexpr uint32_t total      = perBinding * 9;
    RHIBuffer*         materials =
        device->CreateStorageBuffer(static_cast<uint32_t>(materialData.size() * sizeof(sg::MaterialData)),
                                    reinterpret_cast<const uint8_t*>(materialData.data()), "spatial_materials");
    buffers.push_back(materials);
    ASSERT_NE(materials, nullptr);
    RHIBufferCreateInfo info;
    info.size         = total * sizeof(Vec4);
    info.allocateType = RHIBufferAllocateType::eGPU;
    info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer);
    RHIBuffer* output = device->CreateBuffer(info);
    buffers.push_back(output);
    info.allocateType = RHIBufferAllocateType::eCPURead;
    info.usageFlags.Clear();
    info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);
    RHIBuffer* readback = device->CreateBuffer(info);
    buffers.push_back(readback);
    ASSERT_NE(output, nullptr);
    ASSERT_NE(readback, nullptr);
    RHISamplerCreateInfo sampler = RHISamplerCreateInfo::CreateLinearRepeat();
    sampler.repeatU = sampler.repeatV = sampler.repeatW = RHISamplerRepeatMode::eClampToEdge;
    RHISampler*  sampling                               = device->CreateSampler(sampler);
    RenderGraph& graph                                  = *device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph.Begin());
    RDGComputePassDesc pass;
    pass.SetShaderProgramName("SpatialMaterialSamplingSP");
    pass.BindStorageBuffer("Results", output, RDGContentGuarantee::eFullWrite);
    pass.BindStorageBuffer("MaterialBuffer", materials);
    BindSceneTextureArray(pass, sampling, views, {sampling});
    graph.AddComputePass(std::move(pass)).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
        encoder.Dispatch((total + 63) / 64, 1, 1);
    });
    graph.AddTransferPass("spatial_readback").CopyBuffer(output, readback, {0, 0, total * sizeof(Vec4)});
    ASSERT_TRUE(graph.End()) << graph.GetResult().message;
    ASSERT_TRUE(device->ExecuteRenderGraph(graph)) << graph.GetResult().message;
    device->FlushRHIThread();
    device->WaitForIdle();
    const uint8_t* mapped = readback->Map();
    ASSERT_NE(mapped, nullptr);
    HeapVector<Vec4> values(total);
    std::memcpy(values.data(), mapped, total * sizeof(Vec4));
    readback->Unmap();
    uint32_t differences      = 0;
    uint32_t alphaDifferences = 0;
    float    maxDifference    = 0;
    for (uint32_t sample = 0; sample < perBinding * 4; ++sample)
    {
        const Vec4 a = values[sample];
        const Vec4 b = values[sample + perBinding * 4];
        for (uint32_t channel = 0; channel < 4; ++channel)
        {
            ASSERT_TRUE(std::isfinite(a[channel]) && std::isfinite(b[channel]));
            differences   += a[channel] != b[channel] ? 1u : 0u;
            maxDifference  = std::max(maxDifference, std::abs(a[channel] - b[channel]));
        }
        alphaDifferences += (a.a >= 0.5f) != (b.a >= 0.5f) ? 1u : 0u;
    }
    RecordProperty("float_channel_differences", differences);
    RecordProperty("alpha_decision_differences", alphaDifferences);
    RecordProperty("max_float_difference", std::to_string(maxDifference));
    EXPECT_EQ(differences, 0u);
    EXPECT_EQ(alphaDifferences, 0u);
    EXPECT_NE(values[0], values[perBinding * 2]);
    EXPECT_EQ(std::memcmp(values.data(), values.data() + perBinding * 8, perBinding * sizeof(Vec4)), 0);
    graph.Reset();
}

TEST_P(ArraySceneTextureIntegrationTest, SpatialQualificationFailureAllowsNativeRetry)
{
    ASSERT_TRUE(device->PrepareForSceneReplacement());
    SpatialQualificationPipelineFailure::calls = 0;
    {
        test::ScopedVulkanCall<PFN_vkCreateComputePipelines> failure(vkCreateComputePipelines,
                                                                     SpatialQualificationPipelineFailure::Create);
        EXPECT_FALSE(device->QualifySpatialTextureSampling());
        EXPECT_EQ(device->GetTextureSamplingParameters().y, 0);
        EXPECT_EQ(SpatialQualificationPipelineFailure::calls, 1u);
    }
    EXPECT_FALSE(device->AreSubmissionsBlocked());
    ASSERT_TRUE(device->QualifySpatialTextureSampling());
    EXPECT_NE(device->GetTextureSamplingParameters().y, 0);
    RecordProperty("rounding_rule", static_cast<int>(device->GetTextureSamplingParameters().y));
    RecordProperty("device_name", device->GetGPUInfo().deviceName.data());
}

#include "NormalSceneTextureIntegrationTests.inl"

INSTANTIATE_TEST_SUITE_P(Execution,
                         ArraySceneTextureIntegrationTest,
                         testing::Values(RHIExecutionMode::eInline, RHIExecutionMode::eThreaded));
} // namespace
