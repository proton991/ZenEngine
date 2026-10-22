#include "Graphics/RHI/RHIShaderUtil.h"
#include "Graphics/VulkanRHI/VulkanPipeline.h"
#include <filesystem>
#include <limits>
#include "Platform/FileSystem.h"
#include <gtest/gtest.h>
#include "AssetLib/Types.h"
#include "Graphics/Shared/VoxelGI.h"

using namespace zen;

namespace
{
// These tests exercise parsing and layout rules without a device, so no device set limit applies.
constexpr uint32_t kNoDeviceSetLimit = std::numeric_limits<uint32_t>::max();
} // namespace

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

        if (heap.name == NameID("uTexture2DHeap"))
        {
            EXPECT_EQ(heap.binding, ZEN_BINDLESS_HEAP_BINDING_TEXTURE2D);

            EXPECT_EQ(heap.type, RHIShaderResourceType::eTexture);
        }
        else
        {
            EXPECT_EQ(heap.name, NameID("uSamplerHeap"));

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

    EXPECT_EQ(uv.name, NameID("UVBuffer"));

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

    EXPECT_EQ(uv.name, NameID("UVBuffer"));

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

    for (zen::SmallVector<zen::RHIShaderResourceDescriptor> const& set : info.SRDTable)
    {
        for (zen::RHIShaderResourceDescriptor const& binding : set)
        {
            if (binding.name == name)
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
    const RHIShaderGroupInfo geometry = ReflectStage(RHIShaderStage::eFragment, "VoxelGI/voxelization.frag.spv");

    ExpectWritable(geometry, "voxelOwner", true);

    for (NameID name : {"voxelNormal", "voxelEmissive", "staticVoxelFlag"})
    {
        EXPECT_EQ(FindBinding(geometry, name), nullptr);
    }

    const RHIShaderGroupInfo inject = ReflectStage(RHIShaderStage::eCompute, "VoxelGI/inject_radiance.comp.spv");

    ExpectWritable(inject, "voxelNormal", false);

    ExpectWritable(inject, "voxelRadiance", true);

    ExpectWritable(inject, "voxelEmissive", false);

    // Class owners are elected beside the owners; the light-visibility cache is written by injection
    // and only read by the trace.
    ExpectWritable(ReflectStage(RHIShaderStage::eCompute, "VoxelGI/voxelization.comp.spv"), "voxelClassOwner", true);

    ExpectWritable(geometry, "voxelClassOwner", true);

    ExpectWritable(ReflectStage(RHIShaderStage::eCompute, "VoxelGI/clear_owners.comp.spv"), "voxelClassOwner", true);

    const RHIShaderGroupInfo meshInject = ReflectStage(RHIShaderStage::eCompute, "VoxelGI/inject_hardware_radiance.comp.spv");

    ExpectWritable(meshInject, "voxelClassOwner", false);

    ExpectWritable(meshInject, "LightVisibilityCache", true);

    // Every injection writes the cache, so the graph knows its contents even without mesh light visibility.
    ExpectWritable(inject, "LightVisibilityCache", true);

    const RHIShaderGroupInfo trace = ReflectStage(RHIShaderStage::eCompute, "VoxelGI/hybrid_trace_hardware.comp.spv");

    for (NameID name : {"HitLightCache", "HitLightCachePrevious", "hitReflectance"})
    {
        ExpectWritable(trace, name, false);
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
HeapVector<uint8_t> LoadReflectionFixture(const char* name)
{
    HeapVector<uint8_t> bytes;

    std::ifstream file(std::string(RDG_REFLECTION_TEST_PATH) + name, std::ios::binary | std::ios::ate);

    EXPECT_TRUE(file.is_open());

    if (file.is_open())
    {
        const std::ifstream::pos_type size = file.tellg();

        EXPECT_GT(size, 0);

        if (size > 0)
        {
            bytes.resize(static_cast<size_t>(size));

            file.seekg(0);

            file.read(reinterpret_cast<char*>(bytes.data()), size);

            EXPECT_TRUE(file.good());
        }
    }

    return bytes;
}

void ExpectAccess(const RHIShaderGroupInfo& info, NameID name, bool readable, bool writable)
{
    SCOPED_TRACE(name.CStr());

    const RHIShaderResourceDescriptor* binding = FindBinding(info, name);

    ASSERT_NE(binding, nullptr);

    EXPECT_EQ(binding->readable, readable);

    EXPECT_EQ(binding->writable, writable);
}
} // namespace

TEST(ShaderReflectionTests, SharedSpecializationIdsMergeStagesAndKeepNewConstants)
{
    RefCountPtr<RHIShaderGroupSPIRV> spirv = MakeRefCountPtr<RHIShaderGroupSPIRV>();

    spirv->SetStageSPIRV(RHIShaderStage::eVertex, LoadReflectionFixture("pipeline.vert.spv"));

    spirv->SetStageSPIRV(RHIShaderStage::eFragment, LoadReflectionFixture("pipeline.frag.spv"));

    RHIShaderGroupInfo info{};

    EXPECT_TRUE(RHIShaderUtil::ReflectShaderGroupInfo(spirv, kNoDeviceSetLimit, info));

    ASSERT_EQ(info.specializationConstants.size(), 3u);

    std::ranges::sort(info.specializationConstants, {}, &RHIShaderSpecializationConstant::constantId);

    for (uint32_t id = 0; id < 3; ++id)
    {
        const RHIShaderSpecializationConstant& constant = info.specializationConstants[id];

        EXPECT_EQ(constant.constantId, id);

        EXPECT_TRUE(constant.stages.HasFlag(RHIShaderStageFlagBits::eFragment));

        EXPECT_EQ(constant.stages.HasFlag(RHIShaderStageFlagBits::eVertex), id < 2);
    }

    EXPECT_EQ(info.specializationConstants[0].type, RHIShaderSpecializationConstantType::eBool);

    EXPECT_TRUE(info.specializationConstants[0].bits);

    EXPECT_EQ(info.specializationConstants[1].type, RHIShaderSpecializationConstantType::eInt);

    EXPECT_EQ(info.specializationConstants[1].bits, static_cast<uint32_t>(-7));

    EXPECT_EQ(info.specializationConstants[2].type, RHIShaderSpecializationConstantType::eFloat);

    EXPECT_FLOAT_EQ(std::bit_cast<float>(info.specializationConstants[2].bits), 1.25f);
}

TEST(ShaderReflectionTests, StorageQualifiersAndMemberAccessAreReflectedFromSPIRV)
{
    HeapVector<uint8_t> bytes = LoadReflectionFixture("rdg_access.comp.spv");

    ASSERT_FALSE(bytes.empty());

    RefCountPtr<zen::RHIShaderGroupSPIRV> spirv = MakeRefCountPtr<RHIShaderGroupSPIRV>();

    spirv->SetStageSPIRV(RHIShaderStage::eCompute, std::move(bytes));

    RHIShaderGroupInfo info;

    EXPECT_TRUE(RHIShaderUtil::ReflectShaderGroupInfo(spirv, kNoDeviceSetLimit, info));

    ExpectAccess(info, "ReadBlock", true, false);

    ExpectAccess(info, "WriteBlock", false, true);

    ExpectAccess(info, "MixedBlock", true, true);

    ExpectAccess(info, "UnqualifiedBlock", true, true);

    ExpectAccess(info, "readImage", true, false);

    ExpectAccess(info, "writeImage", false, true);

    ExpectAccess(info, "readWriteImage", true, true);

    for (NameID name : {"BufferArray", "UniformArray"})
    {
        SCOPED_TRACE(name.CStr());

        const RHIShaderResourceDescriptor* binding = FindBinding(info, name);

        ASSERT_NE(binding, nullptr);

        EXPECT_EQ(binding->arraySize, 2u);

        if (binding->type == RHIShaderResourceType::eUniformBuffer)
        {
            EXPECT_GE(binding->blockSize, 4u);
        }
        else
        {
            EXPECT_EQ(binding->blockSize,
                      0u); // SPIRV-Reflect reports storage blocks as runtime-sized.
        }

        ExpectAccess(info, name, true, false);
    }
}

TEST(ShaderReflectionTests, SharedBindingAccessIsUnionedInEitherStageMergeOrder)
{
    HeapVector<uint8_t> vertex   = LoadReflectionFixture("rdg_shared.vert.spv");

    HeapVector<uint8_t> fragment = LoadReflectionFixture("rdg_shared.frag.spv");

    ASSERT_FALSE(vertex.empty());

    ASSERT_FALSE(fragment.empty());

    RHIShaderResourceDescriptor descriptors[2];

    for (uint32_t i = 0; i < 2; ++i)
    {
        RefCountPtr<zen::RHIShaderGroupSPIRV> spirv = MakeRefCountPtr<RHIShaderGroupSPIRV>();

        spirv->SetStageSPIRV(i == 0 ? RHIShaderStage::eVertex : RHIShaderStage::eFragment,
                             i == 0 ? HeapVector<uint8_t>(vertex) : HeapVector<uint8_t>(fragment));

        RHIShaderGroupInfo info;

        EXPECT_TRUE(RHIShaderUtil::ReflectShaderGroupInfo(spirv, kNoDeviceSetLimit, info));

        ASSERT_EQ(info.SRDTable.size(), 1u);

        ASSERT_EQ(info.SRDTable[0].size(), 1u);

        descriptors[i] = info.SRDTable[0][0];

        EXPECT_EQ(descriptors[i].readable, i == 0);

        EXPECT_EQ(descriptors[i].writable, i == 1);
    }

    for (bool reverse : {false, true})
    {
        RHIShaderGroupInfo info;

        for (uint32_t n = 0; n < 2; ++n)
        {
            const uint32_t i = reverse ? 1 - n : n;

            MergeOrAddSRDs(i == 0 ? RHIShaderStage::eVertex : RHIShaderStage::eFragment, descriptors[i], kNoDeviceSetLimit,
                           info);
        }

        ASSERT_EQ(info.SRDTable.size(), 1u);

        ASSERT_EQ(info.SRDTable[0].size(), 1u);

        zen::RHIShaderResourceDescriptor const& binding = info.SRDTable[0][0];

        EXPECT_TRUE(binding.readable);

        EXPECT_TRUE(binding.writable);

        EXPECT_TRUE(binding.stageFlags.HasFlag(RHIShaderStageFlagBits::eVertex));

        EXPECT_TRUE(binding.stageFlags.HasFlag(RHIShaderStageFlagBits::eFragment));
    }

    RefCountPtr<zen::RHIShaderGroupSPIRV> both = MakeRefCountPtr<RHIShaderGroupSPIRV>();

    both->SetStageSPIRV(RHIShaderStage::eVertex, std::move(vertex));

    both->SetStageSPIRV(RHIShaderStage::eFragment, std::move(fragment));

    RHIShaderGroupInfo info;

    EXPECT_TRUE(RHIShaderUtil::ReflectShaderGroupInfo(both, kNoDeviceSetLimit, info));

    ExpectAccess(info, "SharedBuffer", true, true);
}

TEST(ShaderReflectionTests, NestedAndIncompleteBlockMetadataCannotHideAccess)
{
    SpvReflectBlockVariable leaves[2]{};

    leaves[0].decoration_flags = SPV_REFLECT_DECORATION_NON_WRITABLE;

    leaves[1].decoration_flags = SPV_REFLECT_DECORATION_NON_READABLE;

    SpvReflectBlockVariable nested{};

    nested.member_count = 2;

    nested.members      = leaves;

    SpvReflectDescriptorBinding binding{};

    binding.descriptor_type        = SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER;

    binding.name                   = "nested";

    binding.block.member_count     = 1;

    binding.block.members          = &nested;

    binding.block.decoration_flags = SPV_REFLECT_DECORATION_NON_WRITABLE;

    RHIShaderResourceDescriptor result;

    ParseSpvReflectDescriptorBinding(binding, result);

    EXPECT_TRUE(result.readable);

    EXPECT_TRUE(result.writable);

    nested.decoration_flags = SPV_REFLECT_DECORATION_NON_READABLE;

    ParseSpvReflectDescriptorBinding(binding, result);

    EXPECT_FALSE(result.readable);

    EXPECT_TRUE(result.writable);

    binding.block.members = nullptr;

    ParseSpvReflectDescriptorBinding(binding, result);

    EXPECT_TRUE(result.readable);

    EXPECT_TRUE(result.writable);
}

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

TEST(ShaderReflectionTests, GeometryVoxelizationKeepsTheFullSceneVertexStride)
{
    const RHIShaderGroupInfo voxel   = ReflectStage(RHIShaderStage::eVertex, "VoxelGI/voxelization.vert.spv");

    const RHIShaderGroupInfo gbuffer = ReflectStage(RHIShaderStage::eVertex, "SceneRenderer/offscreen.vert.spv");

    EXPECT_EQ(voxel.vertexBindingStride, sizeof(asset::Vertex));

    EXPECT_EQ(gbuffer.vertexBindingStride, sizeof(asset::Vertex));

    EXPECT_EQ(voxel.pushConstants.size, 4u * sizeof(uint32_t));
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

TEST(ShaderReflectionTests, RayShadowedForwardLayersReadTheDeferredReceiversShadowRays)
{
    for (const char* path :
         {"SceneRenderer/forward_material_shadows.frag.spv", "SceneRenderer/forward_material_shadows_capture.frag.spv",
          "SceneRenderer/forward_material_shadows_counts.frag.spv", "SceneRenderer/forward_material_software_shadows.frag.spv",
          "SceneRenderer/forward_material_software_shadows_capture.frag.spv",
          "SceneRenderer/forward_material_software_shadows_counts.frag.spv"})
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

    // Without ray shadows there are no shadow rays to reuse.
    const RHIShaderGroupInfo hybrid = ReflectStage(RHIShaderStage::eFragment, "SceneRenderer/forward_material_hybrid.frag.spv");

    EXPECT_EQ(FindBinding(hybrid, "receiverLightVisibility"), nullptr);
}

// Deferred lighting writes the receivers' shadow-ray results to color location 1 only in the
// visibility variants, which DeferredLightingRenderer selects when a decal will read them.
TEST(ShaderReflectionTests, OnlyVisibilityVariantsExportDeferredShadowRays)
{
    for (const char* name :
         {"hybrid_gi_shadows", "hybrid_gi_shadows_capture", "hybrid_gi_shadows_counts", "hybrid_gi_software_shadows",
          "hybrid_gi_software_shadows_capture", "hybrid_gi_software_shadows_counts"})
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
          "VoxelGI/sky_irradiance.comp.spv", "VoxelGI/inject_radiance.comp.spv", "VoxelGI/filter_albedo.comp.spv",
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

TEST(ShaderReflectionTests, CorruptInputDoesNotPublishPartialReflection)
{
    RHIShaderGroupSPIRVPtr spirv = MakeRefCountPtr<RHIShaderGroupSPIRV>();

    spirv->SetStageSPIRV(RHIShaderStage::eCompute, HeapVector<uint8_t>{0, 1, 2, 3});

    RHIShaderGroupInfo info;

    info.vertexBindingStride = 123;

    EXPECT_FALSE(RHIShaderUtil::ReflectShaderGroupInfo(spirv, kNoDeviceSetLimit, info));

    EXPECT_EQ(info.vertexBindingStride, 123u);
}

TEST(ShaderReflectionTests, UnsupportedMetadataFailsBeforePublication)
{
    RHIShaderGroupInfo info;

    SpvReflectShaderModule module{};

    module.push_constant_block_count = 2;

    EXPECT_FALSE(ParseSpvPushConstants(RHIShaderStage::eCompute, &module, info));

    SpvReflectTypeDescription type{};

    type.op                          = SpvOpTypeFloat;

    type.traits.numeric.scalar.width = 64;

    double value                     = 1.0;

    SpvReflectSpecializationConstant constant{};

    constant.type_description   = &type;

    constant.default_value      = &value;

    constant.default_value_size = sizeof(value);

    module.spec_constant_count  = 1;

    module.spec_constants       = &constant;

    EXPECT_FALSE(ParseSpvSpecializationConstant(RHIShaderStage::eCompute, &module, info));

    EXPECT_TRUE(info.specializationConstants.empty());

    for (SpvReflectDescriptorType unsupported :
         {SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC})
    {
        SpvReflectDescriptorBinding binding{};

        binding.descriptor_type = unsupported;

        RHIShaderResourceDescriptor descriptor{};

        EXPECT_FALSE(ParseSpvReflectDescriptorBinding(binding, descriptor));
    }
}

TEST(ShaderReflectionTests, AccelerationStructureUsesAnOrdinaryReadOnlyDescriptor)
{
    SpvReflectDescriptorBinding binding{};
    binding.descriptor_type = SPV_REFLECT_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    binding.set             = 6;
    binding.binding         = 2;
    RHIShaderResourceDescriptor descriptor{};
    ASSERT_TRUE(ParseSpvReflectDescriptorBinding(binding, descriptor));
    EXPECT_EQ(descriptor.type, RHIShaderResourceType::eAccelerationStructure);
    EXPECT_EQ(descriptor.set, 6u);
    EXPECT_EQ(descriptor.binding, 2u);
    EXPECT_EQ(descriptor.arraySize, 1u);
    EXPECT_TRUE(descriptor.readable);
    EXPECT_FALSE(descriptor.writable);
    EXPECT_FALSE(descriptor.bindless);
}

TEST(ShaderReflectionTests, ReflectionRejectsSetsAtOrBeyondTheDeviceLimit)
{
    RHIShaderResourceDescriptor descriptor{};
    descriptor.set  = 5;
    descriptor.type = RHIShaderResourceType::eStorageBuffer;
    RHIShaderGroupInfo rejected{};
    EXPECT_FALSE(MergeOrAddSRDs(RHIShaderStage::eCompute, descriptor, 5, rejected));
    // A rejected index never sizes the table, so a corrupt index cannot grow it.
    EXPECT_TRUE(rejected.SRDTable.empty());
    RHIShaderGroupInfo accepted{};
    EXPECT_TRUE(MergeOrAddSRDs(RHIShaderStage::eCompute, descriptor, 6, accepted));
    EXPECT_EQ(accepted.SRDTable.size(), 6u);
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
