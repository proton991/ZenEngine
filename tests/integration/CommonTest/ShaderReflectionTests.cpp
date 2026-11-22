#include <spirv_reflect.h>
#include "ZenRHI/RHIShaderUtil.h"
#include "Vulkan/VulkanPipeline.h"
#include <filesystem>
#include <limits>
#include "Platform/FileSystem.h"
#include <gtest/gtest.h>
#include "AssetLib/Types.h"
#include "Graphics/Shared/VoxelGI.h"
#include "SceneGraph/Material.h"

using namespace zen;

namespace
{
// These tests exercise parsing and layout rules without a device, so no device set limit applies.
constexpr uint32_t kNoDeviceSetLimit = std::numeric_limits<uint32_t>::max();
} // namespace

TEST(ShaderReflectionTests, RasterAndGIKeepMaterialNormalEncodingLayout)
{
    for (const char* path :
         {"SceneRenderer/offscreen.frag.spv", "VoxelGI/voxelization.comp.spv", "VoxelGI/distance_field_coverage.comp.spv"})
    {
        SCOPED_TRACE(path);
        const HeapVector<uint8_t> code = platform::FileSystem::LoadSpvFile(path);
        ASSERT_FALSE(code.empty());
        SpvReflectShaderModule module{};
        ASSERT_EQ(spvReflectCreateShaderModule(code.size(), code.data(), &module), SPV_REFLECT_RESULT_SUCCESS);
        bool found = false;
        for (uint32_t index = 0; index < module.descriptor_binding_count; ++index)
        {
            const SpvReflectDescriptorBinding& binding = module.descriptor_bindings[index];
            if (binding.type_description != nullptr && binding.type_description->type_name != nullptr
                && std::strcmp(binding.type_description->type_name, "MaterialBuffer") == 0)
            {
                found = true;
                EXPECT_EQ(binding.block.member_count, 1u);
                if (binding.block.member_count == 1)
                {
                    const SpvReflectBlockVariable& material = binding.block.members[0];
                    EXPECT_NE(material.type_description, nullptr);
                    if (material.type_description != nullptr)
                    {
                        EXPECT_EQ(material.type_description->traits.array.stride, sizeof(sg::MaterialData));
                    }
                    bool foundFlags    = false;
                    bool foundFeatures = false;
                    bool foundSampling = false;
                    for (uint32_t member = 0; member < material.member_count; ++member)
                    {
                        const SpvReflectBlockVariable& field = material.members[member];
                        if (field.name != nullptr && std::strcmp(field.name, "normalEncoding") == 0)
                        {
                            foundFlags = true;
                            EXPECT_EQ(field.offset, offsetof(sg::MaterialData, normalEncoding));
                            EXPECT_EQ(field.size, 16u);
                        }
                        if (field.name != nullptr && std::strcmp(field.name, "featureTextures") == 0)
                        {
                            foundFeatures = true;
                            EXPECT_EQ(field.offset, offsetof(sg::MaterialData, featureTextures));
                        }
                    }
                    for (uint32_t member = 0; member < material.member_count; ++member)
                    {
                        const SpvReflectBlockVariable& field = material.members[member];
                        if (field.name != nullptr && std::strcmp(field.name, "textureSampling") == 0)
                        {
                            foundSampling = true;
                            EXPECT_EQ(field.offset, offsetof(sg::MaterialData, textureSampling));
                            EXPECT_EQ(field.size, 16u);
                        }
                        if (field.name != nullptr && std::strcmp(field.name, "textureTransforms") == 0)
                        {
                            EXPECT_EQ(field.type_description->traits.array.stride, sizeof(sg::TextureTransformData));
                            ASSERT_EQ(field.member_count, 3u);
                            EXPECT_EQ(field.members[2].offset, offsetof(sg::TextureTransformData, atlasRegion));
                        }
                    }
                    EXPECT_TRUE(foundSampling);
                    EXPECT_TRUE(foundFlags);
                    EXPECT_TRUE(foundFeatures);
                }
            }
        }
        EXPECT_TRUE(found);
        spvReflectDestroyShaderModule(&module);
    }
}

TEST(ShaderReflectionTests, SceneShadersKeepEveryBindingInItsDescriptorSet)
{
    RefCountPtr<zen::RHIShaderGroupSPIRV> spirv = MakeRefCountPtr<RHIShaderGroupSPIRV>();

    spirv->SetStageSPIRV(RHIShaderStage::eVertex, platform::FileSystem::LoadSpvFile("SceneRenderer/offscreen.vert.spv"));

    spirv->SetStageSPIRV(RHIShaderStage::eFragment, platform::FileSystem::LoadSpvFile("SceneRenderer/offscreen.frag.spv"));

    RHIShaderGroupInfo info{};

    EXPECT_TRUE(RHIShaderUtil::ReflectShaderGroupInfo(spirv, kNoDeviceSetLimit, info));

    ASSERT_EQ(info.SRDTable.size(), 2u);

    ASSERT_EQ(info.SRDTable[0].size(), 2u);

    ASSERT_EQ(info.SRDTable[1].size(), 4u);

    for (const RHIShaderResourceDescriptor& heap : info.SRDTable[0])
    {
        EXPECT_EQ(heap.set, kGlobalBindlessHeapIndex);

        EXPECT_TRUE(heap.bindless);

        EXPECT_TRUE(heap.stageFlags.HasFlag(RHIShaderStageFlagBits::eVertex));

        EXPECT_TRUE(heap.stageFlags.HasFlag(RHIShaderStageFlagBits::eFragment));

        if (heap.name == "uTexture2DHeap")
        {
            EXPECT_EQ(heap.binding, ZEN_BINDLESS_HEAP_BINDING_TEXTURE2D);

            EXPECT_EQ(heap.type, RHIShaderResourceType::eTexture);
        }
        else
        {
            EXPECT_EQ(heap.name, "uSamplerHeap");

            EXPECT_EQ(heap.binding, ZEN_BINDLESS_HEAP_BINDING_SAMPLER);

            EXPECT_EQ(heap.type, RHIShaderResourceType::eSampler);
        }
    }

    for (uint32_t binding = 0; binding < 4; ++binding)
    {
        EXPECT_EQ(info.SRDTable[1][binding].set, 1u);

        EXPECT_EQ(info.SRDTable[1][binding].binding, binding);
    }

    const RHIShaderResourceDescriptor& uv = info.SRDTable[1][3];

    EXPECT_EQ(uv.name, "UVBuffer");

    EXPECT_EQ(uv.type, RHIShaderResourceType::eStorageBuffer);

    EXPECT_FALSE(uv.writable);

    EXPECT_TRUE(uv.stageFlags.HasFlag(RHIShaderStageFlagBits::eVertex));

    EXPECT_FALSE(uv.stageFlags.HasFlag(RHIShaderStageFlagBits::eFragment));
}

TEST(ShaderReflectionTests, VoxelShaderInitializesAllDescriptorSets)
{
    RefCountPtr<zen::RHIShaderGroupSPIRV> spirv = MakeRefCountPtr<RHIShaderGroupSPIRV>();

    spirv->SetStageSPIRV(RHIShaderStage::eCompute, platform::FileSystem::LoadSpvFile("VoxelGI/voxelization.comp.spv"));

    RHIShaderGroupInfo info{};

    EXPECT_TRUE(RHIShaderUtil::ReflectShaderGroupInfo(spirv, kNoDeviceSetLimit, info));

    // Set 4 holds the owner and class-owner volumes.
    const uint32_t bindingCounts[] = {2, 1, 0, 6, 2};

    ASSERT_EQ(info.SRDTable.size(), std::size(bindingCounts));

    for (uint32_t set = 0; set < std::size(bindingCounts); ++set)
    {
        ASSERT_EQ(info.SRDTable[set].size(), bindingCounts[set]);

        for (zen::RHIShaderResourceDescriptor const& descriptor : info.SRDTable[set])
        {
            EXPECT_EQ(descriptor.set, set);

            EXPECT_TRUE(descriptor.stageFlags.HasFlag(RHIShaderStageFlagBits::eCompute));
        }
    }

    const RHIShaderResourceDescriptor& uv = info.SRDTable[3][5];

    EXPECT_EQ(uv.name, "UVBuffer");

    EXPECT_EQ(uv.binding, 5u);

    EXPECT_EQ(uv.type, RHIShaderResourceType::eStorageBuffer);

    EXPECT_FALSE(uv.writable);
}

TEST(ShaderReflectionTests, SparseDescriptorSetsPreserveTheirShaderSetNumbers)
{
    RefCountPtr<zen::RHIShaderGroupSPIRV> spirv = MakeRefCountPtr<RHIShaderGroupSPIRV>();

    spirv->SetStageSPIRV(RHIShaderStage::eFragment, platform::FileSystem::LoadSpvFile("VoxelGI/voxel_vis.frag.spv"));

    RHIShaderGroupInfo info{};

    EXPECT_TRUE(RHIShaderUtil::ReflectShaderGroupInfo(spirv, kNoDeviceSetLimit, info));

    ASSERT_EQ(info.SRDTable.size(), 4u);

    ASSERT_EQ(info.SRDTable[0].size(), 2u);

    EXPECT_TRUE(info.SRDTable[0][0].bindless);

    EXPECT_TRUE(info.SRDTable[0][1].bindless);

    EXPECT_TRUE(info.SRDTable[1].empty());

    EXPECT_TRUE(info.SRDTable[2].empty());

    ASSERT_EQ(info.SRDTable[3].size(), 1u);

    EXPECT_EQ(info.SRDTable[3][0].set, 3u);

    EXPECT_EQ(info.SRDTable[3][0].binding, 0u);

    EXPECT_EQ(info.SRDTable[3][0].type, RHIShaderResourceType::eStorageBuffer);
}

namespace
{
RHIShaderGroupInfo ReflectStage(RHIShaderStage stage, const char* path)
{
    RefCountPtr<zen::RHIShaderGroupSPIRV> spirv = MakeRefCountPtr<RHIShaderGroupSPIRV>();

    spirv->SetStageSPIRV(stage, platform::FileSystem::LoadSpvFile(path));

    RHIShaderGroupInfo info{};

    EXPECT_TRUE(RHIShaderUtil::ReflectShaderGroupInfo(spirv, kNoDeviceSetLimit, info));

    return info;
}

const RHIShaderResourceDescriptor* FindBinding(const RHIShaderGroupInfo& info, NameID name)
{
    const RHIShaderResourceDescriptor* pResult = nullptr;

    for (std::vector<zen::RHIShaderResourceDescriptor> const& set : info.SRDTable)
    {
        for (zen::RHIShaderResourceDescriptor const& binding : set)
        {
            if (binding.name == std::string_view(name))
            {
                pResult = &binding;

                break;
            }
        }

        if (pResult != nullptr)
        {
            break;
        }
    }

    return pResult;
}

void ExpectWritable(const RHIShaderGroupInfo& info, NameID name, bool writable)
{
    SCOPED_TRACE(name.CStr());

    const RHIShaderResourceDescriptor* binding = FindBinding(info, name);

    ASSERT_NE(binding, nullptr);

    EXPECT_EQ(binding->writable, writable);
}
} // namespace

TEST(ShaderReflectionTests, VoxelDrawReadersAndPreDrawWritersKeepDistinctAccess)
{
    const RHIShaderGroupInfo draw = ReflectStage(RHIShaderStage::eFragment, "VoxelGI/voxel_vis.frag.spv");

    ExpectWritable(draw, "InstanceColorBuffer", false);

    const RHIShaderGroupInfo vertex = ReflectStage(RHIShaderStage::eVertex, "VoxelGI/voxel_vis.vert.spv");

    ExpectWritable(vertex, "InstanceBuffer", false);

    const RHIShaderGroupInfo preDraw = ReflectStage(RHIShaderStage::eCompute, "VoxelGI/voxel_pre_draw.comp.spv");

    ExpectWritable(preDraw, "voxelTexture", false);

    ExpectWritable(preDraw, "InstancePositionBuffer", true);

    ExpectWritable(preDraw, "InstanceColorBuffer", true);

    ExpectWritable(preDraw, "IndirectBuffer", true);
}

TEST(ShaderReflectionTests, ComputeVoxelizerBindingsMatchProducerAndConsumerAccess)
{
    const RHIShaderGroupInfo producer = ReflectStage(RHIShaderStage::eCompute, "VoxelGI/voxelization.comp.spv");

    const RHIShaderGroupInfo resolve  = ReflectStage(RHIShaderStage::eCompute, "VoxelGI/resolve_surface.comp.spv");

    for (const RHIShaderGroupInfo* shader : {&producer, &resolve})
    {
        for (NameID name : {"VertexBuffer", "IndexBuffer", "NodeBuffer", "TriangleRecords", "MaterialBuffer"})
        {
            ExpectWritable(*shader, name, false);
        }
    }

    ExpectWritable(producer, "voxelOwner", true);

    ExpectWritable(resolve, "voxelOwner", false);

    for (NameID name : {"voxelAlbedo", "voxelNormal", "voxelEmissive"})
    {
        ExpectWritable(resolve, name, true);
    }
}

TEST(ShaderReflectionTests, VoxelAtomicAndRadianceWritesRemainWritable)
{
    const RHIShaderGroupInfo inject = ReflectStage(RHIShaderStage::eCompute, "VoxelGI/inject_radiance.comp.spv");

    ExpectWritable(inject, "voxelNormal", false);

    ExpectWritable(inject, "voxelRadiance", true);

    ExpectWritable(inject, "voxelEmissive", false);

    // Class owners are elected beside the owners; the light-visibility cache is written by injection
    // and only read by the trace.
    ExpectWritable(ReflectStage(RHIShaderStage::eCompute, "VoxelGI/voxelization.comp.spv"), "voxelClassOwner", true);

    ExpectWritable(ReflectStage(RHIShaderStage::eCompute, "VoxelGI/clear_owners.comp.spv"), "voxelClassOwner", true);

    const RHIShaderGroupInfo meshInject = ReflectStage(RHIShaderStage::eCompute, "VoxelGI/inject_mesh_radiance.comp.spv");

    ExpectWritable(meshInject, "voxelClassOwner", false);

    ExpectWritable(meshInject, "LightVisibilityCache", true);

    // Every injection writes the cache, so the graph knows its contents even without mesh light visibility.
    ExpectWritable(inject, "LightVisibilityCache", true);

    const RHIShaderGroupInfo hardwareTrace = ReflectStage(RHIShaderStage::eCompute, "VoxelGI/hybrid_trace_hardware.comp.spv");
    const RHIShaderGroupInfo fieldTrace    = ReflectStage(RHIShaderStage::eCompute, "VoxelGI/hybrid_trace_field.comp.spv");

    // HW hit lighting has no voxel cache; Compute field tracing reads it.
    for (NameID name : {"HitLightCache", "HitLightCachePrevious", "hitReflectance"})
    {
        EXPECT_EQ(FindBinding(hardwareTrace, name), nullptr);
        ExpectWritable(fieldTrace, name, false);
    }

    const RHIShaderGroupInfo reset = ReflectStage(RHIShaderStage::eCompute, "VoxelGI/reset_voxel_texture.comp.spv");

    ExpectWritable(reset, "voxelTexture", true);

    for (const char* path : {"VoxelGI/reset_compute_indirect.comp.spv", "VoxelGI/reset_draw_indirect.comp.spv"})
    {
        ExpectWritable(ReflectStage(RHIShaderStage::eCompute, path), "IndirectBuffer", true);
    }
}

namespace
{
void ExpectAccess(const RHIShaderGroupInfo& info, NameID name, bool readable, bool writable)
{
    SCOPED_TRACE(name.CStr());

    const RHIShaderResourceDescriptor* binding = FindBinding(info, name);

    ASSERT_NE(binding, nullptr);

    EXPECT_EQ(binding->readable, readable);

    EXPECT_EQ(binding->writable, writable);
}
} // namespace

TEST(ShaderReflectionTests, VoxelWriteOnlyBindingsRetainPartialWriteSemantics)
{
    for (const char* path : {"VoxelGI/reset_compute_indirect.comp.spv", "VoxelGI/reset_draw_indirect.comp.spv"})
    {
        ExpectAccess(ReflectStage(RHIShaderStage::eCompute, path), "IndirectBuffer", false, true);
    }

    const RHIShaderGroupInfo preDraw = ReflectStage(RHIShaderStage::eCompute, "VoxelGI/voxel_pre_draw.comp.spv");

    ExpectAccess(preDraw, "InstancePositionBuffer", false, true);

    ExpectAccess(preDraw, "InstanceColorBuffer", false, true);

    ExpectAccess(preDraw, "IndirectBuffer", true, true);

    ExpectAccess(ReflectStage(RHIShaderStage::eCompute, "VoxelGI/reset_voxel_texture.comp.spv"), "voxelTexture", false, true);
}

TEST(ShaderReflectionTests, GBufferKeepsTheFullSceneVertexStride)
{
    const RHIShaderGroupInfo gbuffer = ReflectStage(RHIShaderStage::eVertex, "SceneRenderer/offscreen.vert.spv");

    EXPECT_EQ(gbuffer.vertexBindingStride, sizeof(asset::Vertex));
}

TEST(ShaderReflectionTests, SceneShadowsKeepTheMeshVertexLayoutAndUniformContract)
{
    const RHIShaderGroupInfo shadow = ReflectStage(RHIShaderStage::eVertex, "ShadowMapping/scene_shadow.vert.spv");

    EXPECT_EQ(shadow.vertexBindingStride, sizeof(asset::Vertex));

    const size_t offsets[] = {offsetof(asset::Vertex, pos),     offsetof(asset::Vertex, normal),
                              offsetof(asset::Vertex, tangent), offsetof(asset::Vertex, uv0),
                              offsetof(asset::Vertex, uv1),     offsetof(asset::Vertex, joint0),
                              offsetof(asset::Vertex, weight0), offsetof(asset::Vertex, color)};

    ASSERT_EQ(shadow.vertexInputAttributes.size(), std::size(offsets));

    for (uint32_t index = 0; index < std::size(offsets); ++index)
    {
        EXPECT_EQ(shadow.vertexInputAttributes[index].location, index);

        EXPECT_EQ(shadow.vertexInputAttributes[index].offset, offsets[index]);
    }

    EXPECT_GE(shadow.pushConstants.size, 2u * sizeof(uint32_t));

    ASSERT_NE(FindBinding(shadow, "uShadowFace"), nullptr);

    EXPECT_EQ(FindBinding(shadow, "uShadowFace")->blockSize, 80u);
}

TEST(ShaderReflectionTests, HybridForwardLayersReadDeferredReceiverVisibility)
{
    for (const char* path :
         {"SceneRenderer/forward_material_shadows.frag.spv", "SceneRenderer/forward_material_shadows_capture.frag.spv",
          "SceneRenderer/forward_material_shadows_counts.frag.spv", "SceneRenderer/forward_material_hybrid.frag.spv",
          "SceneRenderer/forward_material_hybrid_capture.frag.spv"})
    {
        SCOPED_TRACE(path);

        const RHIShaderGroupInfo info                 = ReflectStage(RHIShaderStage::eFragment, path);

        const RHIShaderResourceDescriptor* visibility = FindBinding(info, "receiverLightVisibility");

        ASSERT_NE(visibility, nullptr);

        EXPECT_EQ(visibility->type, RHIShaderResourceType::eSamplerWithTexture);

        EXPECT_EQ(visibility->set, 4u);

        EXPECT_EQ(visibility->binding, 4u);

        EXPECT_FALSE(visibility->writable);
    }

    // Hybrid layers reuse exact shadow-map visibility as well as hardware shadow rays.
    // Ordinary forward materials do not consume the deferred receiver's visibility.
    const RHIShaderGroupInfo info = ReflectStage(RHIShaderStage::eFragment, "SceneRenderer/forward_material.frag.spv");
    EXPECT_EQ(FindBinding(info, "receiverLightVisibility"), nullptr);
}

// Deferred lighting writes the receivers' shadow-ray results to color location 1 only in the
// visibility variants, which DeferredLightingRenderer selects when a decal will read them.
TEST(ShaderReflectionTests, OnlyVisibilityVariantsExportDeferredShadowRays)
{
    for (const char* name : {"hybrid_gi_shadows", "hybrid_gi_shadows_capture", "hybrid_gi_shadows_counts"})
    {
        for (bool visibility : {false, true})
        {
            const std::string path = std::string("SceneRenderer/") + name + (visibility ? "_visibility" : "") + ".frag.spv";

            SCOPED_TRACE(path);

            const HeapVector<uint8_t> code = platform::FileSystem::LoadSpvFile(path);

            SpvReflectShaderModule module{};

            ASSERT_EQ(spvReflectCreateShaderModule(code.size(), code.data(), &module), SPV_REFLECT_RESULT_SUCCESS);

            uint32_t count = 0;

            EXPECT_EQ(spvReflectEnumerateOutputVariables(&module, &count, nullptr), SPV_REFLECT_RESULT_SUCCESS);

            HeapVector<SpvReflectInterfaceVariable*> outputs(count, nullptr);

            EXPECT_EQ(spvReflectEnumerateOutputVariables(&module, &count, outputs.data()), SPV_REFLECT_RESULT_SUCCESS);

            bool color      = false;

            bool shadowRays = false;

            for (const SpvReflectInterfaceVariable* output : outputs)
            {
                color      = color || output->location == 0;

                shadowRays = shadowRays || output->location == 1;
            }

            EXPECT_TRUE(color);

            EXPECT_EQ(shadowRays, visibility);

            spvReflectDestroyShaderModule(&module);
        }
    }
}

TEST(ShaderReflectionTests, VoxelMipFilteringDeclaresSeparateReadAndWriteImages)
{
    for (const char* path : {"VoxelGI/filter_albedo.comp.spv", "VoxelGI/filter_radiance.comp.spv"})
    {
        const RHIShaderGroupInfo info = ReflectStage(RHIShaderStage::eCompute, path);

        ExpectWritable(info, "sourceVolume", false);

        ExpectWritable(info, "targetVolume", true);

        ASSERT_NE(FindBinding(info, "sourceVolume"), nullptr);

        EXPECT_EQ(FindBinding(info, "sourceVolume")->type, RHIShaderResourceType::eImage);
    }
}

TEST(ShaderReflectionTests, VoxelVolumeWorkgroupsExposeSpecializationAndSafeDefaults)
{
    for (const char* path :
         {"VoxelGI/clear_owners.comp.spv", "VoxelGI/resolve_albedo.comp.spv", "VoxelGI/resolve_surface.comp.spv",
          "VoxelGI/sky_irradiance_field.comp.spv", "VoxelGI/inject_radiance.comp.spv", "VoxelGI/filter_albedo.comp.spv",
          "VoxelGI/filter_radiance.comp.spv", "VoxelGI/voxel_pre_draw.comp.spv"})
    {
        SCOPED_TRACE(path);

        const HeapVector<uint8_t> bytes = platform::FileSystem::LoadSpvFile(path);

        ASSERT_FALSE(bytes.empty());

        SpvReflectShaderModule module{};

        ASSERT_EQ(spvReflectCreateShaderModule(bytes.size(), bytes.data(), &module), SPV_REFLECT_RESULT_SUCCESS);

        const SpvReflectEntryPoint* entry = spvReflectGetEntryPoint(&module, "main");

        EXPECT_NE(entry, nullptr);

        if (entry != nullptr)
        {
            EXPECT_EQ(entry->local_size.x, ZEN_VOXEL_VOLUME_GROUP_SIZE);

            EXPECT_EQ(entry->local_size.y, ZEN_VOXEL_VOLUME_GROUP_SIZE);

            EXPECT_EQ(entry->local_size.z, ZEN_VOXEL_VOLUME_GROUP_SIZE);

            EXPECT_LE(entry->local_size.x, 128u);

            EXPECT_LE(entry->local_size.y, 128u);

            EXPECT_LE(entry->local_size.z, 64u);

            EXPECT_LE(uint64_t(entry->local_size.x) * entry->local_size.y * entry->local_size.z, 128u);
        }

        spvReflectDestroyShaderModule(&module);

        const RHIShaderGroupInfo info = ReflectStage(RHIShaderStage::eCompute, path);

        ASSERT_EQ(info.specializationConstants.size(), 3u);

        std::array<bool, 3> seen{};

        for (const RHIShaderSpecializationConstant& constant : info.specializationConstants)
        {
            ASSERT_LT(constant.constantId, seen.size());

            EXPECT_FALSE(seen[constant.constantId]);

            seen[constant.constantId] = true;

            EXPECT_EQ(constant.type, RHIShaderSpecializationConstantType::eInt);

            EXPECT_EQ(constant.bits, ZEN_VOXEL_VOLUME_GROUP_SIZE);
        }

        EXPECT_TRUE(seen[ZEN_VOXEL_VOLUME_GROUP_X_ID]);

        EXPECT_TRUE(seen[ZEN_VOXEL_VOLUME_GROUP_Y_ID]);

        EXPECT_TRUE(seen[ZEN_VOXEL_VOLUME_GROUP_Z_ID]);
    }
}

namespace
{
RHIShaderStage ShippedShaderStage(const std::filesystem::path& source)
{
    const std::string extension = source.extension().string();
    RHIShaderStage    stage     = RHIShaderStage::eMax;
    if (extension == ".vert")
    {
        stage = RHIShaderStage::eVertex;
    }
    else if (extension == ".frag")
    {
        stage = RHIShaderStage::eFragment;
    }
    else if (extension == ".comp")
    {
        stage = RHIShaderStage::eCompute;
    }
    else if (extension == ".geom")
    {
        stage = RHIShaderStage::eGeometry;
    }
    else if (extension == ".rgen")
    {
        stage = RHIShaderStage::eRayGeneration;
    }
    else if (extension == ".rmiss")
    {
        stage = RHIShaderStage::eMiss;
    }
    else if (extension == ".rchit")
    {
        stage = RHIShaderStage::eClosestHit;
    }
    else if (extension == ".rahit")
    {
        stage = RHIShaderStage::eAnyHit;
    }
    else if (extension == ".rcall")
    {
        stage = RHIShaderStage::eCallable;
    }
    return stage;
}
} // namespace

// Per-stage rules over every compiled source. Cross-stage names are checked when the native
// suite creates every registered program (VulkanPresentationReviewTests.cpp).
TEST(ShaderReflectionTests, ShippedShaderLayoutsRespectGlobalHeapContract)
{
    // Walk the sources the build compiles, so SPIR-V left over from deleted sources is ignored.
    // The build writes Data/Shaders/<path> to SPV_SHADER_PATH/<path>.spv.
    const std::filesystem::path sources = std::filesystem::path(SPV_SHADER_PATH).parent_path().parent_path() / "Shaders";
    std::error_code             error;
    std::filesystem::recursive_directory_iterator entry(sources, error);
    ASSERT_FALSE(error) << sources.string() << ": " << error.message();
    uint32_t checked = 0;
    for (; !error && entry != std::filesystem::recursive_directory_iterator(); entry.increment(error))
    {
        const RHIShaderStage stage = ShippedShaderStage(entry->path());
        if (stage != RHIShaderStage::eMax && entry->is_regular_file(error))
        {
            const std::string relative = std::filesystem::relative(entry->path(), sources, error).generic_string() + ".spv";
            SCOPED_TRACE(relative);
            ASSERT_FALSE(error) << error.message();
            platform::FileLoadError loadError = platform::FileLoadError::eNone;
            RHIShaderGroupSPIRVPtr  spirv     = MakeRefCountPtr<RHIShaderGroupSPIRV>();
            spirv->SetStageSPIRV(stage, platform::FileSystem::LoadSpvFile(relative, &loadError));
            ASSERT_EQ(loadError, platform::FileLoadError::eNone) << "compiled shader is missing";
            RHIShaderGroupInfo info{};
            ASSERT_TRUE(RHIShaderUtil::ReflectShaderGroupInfo(spirv, kNoDeviceSetLimit, info));
            info.name = NameID(relative);
            EXPECT_TRUE(ValidateShaderLayout(info, kNoDeviceSetLimit));
            ++checked;
        }
    }
    EXPECT_FALSE(error) << error.message();
    EXPECT_GT(checked, 0u);
}
