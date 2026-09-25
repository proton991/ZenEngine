#include "Graphics/RHI/RHIShaderUtil.h"
#include "Platform/FileSystem.h"
#include <gtest/gtest.h>
#include "AssetLib/Types.h"
#include "Graphics/Shared/VoxelGI.h"

using namespace zen;

TEST(ShaderReflectionTests, SceneShadersKeepEveryBindingInItsDescriptorSet)
{
    RefCountPtr<zen::RHIShaderGroupSPIRV> spirv = MakeRefCountPtr<RHIShaderGroupSPIRV>();
    spirv->SetStageSPIRV(RHIShaderStage::eVertex,
                         platform::FileSystem::LoadSpvFile("SceneRenderer/offscreen.vert.spv"));
    spirv->SetStageSPIRV(RHIShaderStage::eFragment,
                         platform::FileSystem::LoadSpvFile("SceneRenderer/offscreen.frag.spv"));

    RHIShaderGroupInfo info{};
    RHIShaderUtil::ReflectShaderGroupInfo(spirv, info);

    ASSERT_EQ(info.SRDTable.size(), 2u);
    ASSERT_EQ(info.SRDTable[0].size(), 2u);
    ASSERT_EQ(info.SRDTable[1].size(), 3u);

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

    for (uint32_t binding = 0; binding < 3; ++binding)
    {
        EXPECT_EQ(info.SRDTable[1][binding].set, 1u);
        EXPECT_EQ(info.SRDTable[1][binding].binding, binding);
    }
}

TEST(ShaderReflectionTests, VoxelShaderInitializesAllDescriptorSets)
{
    RefCountPtr<zen::RHIShaderGroupSPIRV> spirv = MakeRefCountPtr<RHIShaderGroupSPIRV>();
    spirv->SetStageSPIRV(RHIShaderStage::eCompute,
                         platform::FileSystem::LoadSpvFile("VoxelGI/voxelization.comp.spv"));

    RHIShaderGroupInfo info{};
    RHIShaderUtil::ReflectShaderGroupInfo(spirv, info);

    const uint32_t bindingCounts[] = {2, 1, 0, 5, 1};
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
}

TEST(ShaderReflectionTests, SparseDescriptorSetsPreserveTheirShaderSetNumbers)
{
    RefCountPtr<zen::RHIShaderGroupSPIRV> spirv = MakeRefCountPtr<RHIShaderGroupSPIRV>();
    spirv->SetStageSPIRV(RHIShaderStage::eFragment,
                         platform::FileSystem::LoadSpvFile("VoxelGI/voxel_vis.frag.spv"));

    RHIShaderGroupInfo info{};
    RHIShaderUtil::ReflectShaderGroupInfo(spirv, info);

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
    RHIShaderUtil::ReflectShaderGroupInfo(spirv, info);

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
    const RHIShaderGroupInfo draw =
        ReflectStage(RHIShaderStage::eFragment, "VoxelGI/voxel_vis.frag.spv");
    ExpectWritable(draw, "InstanceColorBuffer", false);
    const RHIShaderGroupInfo vertex =
        ReflectStage(RHIShaderStage::eVertex, "VoxelGI/voxel_vis.vert.spv");
    ExpectWritable(vertex, "InstanceBuffer", false);
    const RHIShaderGroupInfo preDraw =
        ReflectStage(RHIShaderStage::eCompute, "VoxelGI/voxel_pre_draw.comp.spv");
    ExpectWritable(preDraw, "voxelTexture", false);
    ExpectWritable(preDraw, "InstancePositionBuffer", true);
    ExpectWritable(preDraw, "InstanceColorBuffer", true);
    ExpectWritable(preDraw, "IndirectBuffer", true);
}

TEST(ShaderReflectionTests, ComputeVoxelizerBindingsMatchProducerAndConsumerAccess)
{
    const RHIShaderGroupInfo producer =
        ReflectStage(RHIShaderStage::eCompute, "VoxelGI/voxelization.comp.spv");
    const RHIShaderGroupInfo resolve =
        ReflectStage(RHIShaderStage::eCompute, "VoxelGI/resolve_surface.comp.spv");
    for (const RHIShaderGroupInfo* shader : {&producer, &resolve})
    {
        for (NameID name :
             {"VertexBuffer", "IndexBuffer", "NodeBuffer", "TriangleRecords", "MaterialBuffer"})
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
    const RHIShaderGroupInfo geometry =
        ReflectStage(RHIShaderStage::eFragment, "VoxelGI/voxelization.frag.spv");
    ExpectWritable(geometry, "voxelOwner", true);

    for (NameID name : {"voxelNormal", "voxelEmissive", "staticVoxelFlag"})
    {
        EXPECT_EQ(FindBinding(geometry, name), nullptr);
    }

    const RHIShaderGroupInfo inject =
        ReflectStage(RHIShaderStage::eCompute, "VoxelGI/inject_radiance.comp.spv");
    ExpectWritable(inject, "voxelNormal", false);
    ExpectWritable(inject, "voxelRadiance", true);
    ExpectWritable(inject, "voxelEmissive", false);
    const RHIShaderGroupInfo reset =
        ReflectStage(RHIShaderStage::eCompute, "VoxelGI/reset_voxel_texture.comp.spv");
    ExpectWritable(reset, "voxelTexture", true);

    for (const char* path :
         {"VoxelGI/reset_compute_indirect.comp.spv", "VoxelGI/reset_draw_indirect.comp.spv"})
    {
        ExpectWritable(ReflectStage(RHIShaderStage::eCompute, path), "IndirectBuffer", true);
    }
}

namespace
{
HeapVector<uint8_t> LoadReflectionFixture(const char* name)
{
    HeapVector<uint8_t> bytes;
    std::ifstream file(std::string(RDG_REFLECTION_TEST_PATH) + name,
                       std::ios::binary | std::ios::ate);
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
    auto spirv = MakeRefCountPtr<RHIShaderGroupSPIRV>();
    spirv->SetStageSPIRV(RHIShaderStage::eVertex, LoadReflectionFixture("pipeline.vert.spv"));
    spirv->SetStageSPIRV(RHIShaderStage::eFragment, LoadReflectionFixture("pipeline.frag.spv"));
    RHIShaderGroupInfo info{};
    RHIShaderUtil::ReflectShaderGroupInfo(spirv, info);

    ASSERT_EQ(info.specializationConstants.size(), 3u);
    std::ranges::sort(info.specializationConstants, {},
                      &RHIShaderSpecializationConstant::constantId);
    for (uint32_t id = 0; id < 3; ++id)
    {
        const auto& constant = info.specializationConstants[id];
        EXPECT_EQ(constant.constantId, id);
        EXPECT_TRUE(constant.stages.HasFlag(RHIShaderStageFlagBits::eFragment));
        EXPECT_EQ(constant.stages.HasFlag(RHIShaderStageFlagBits::eVertex), id < 2);
    }
    EXPECT_EQ(info.specializationConstants[0].type, RHIShaderSpecializationConstantType::eBool);
    EXPECT_TRUE(info.specializationConstants[0].boolValue);
    EXPECT_EQ(info.specializationConstants[1].type, RHIShaderSpecializationConstantType::eInt);
    EXPECT_EQ(info.specializationConstants[1].intValue, static_cast<uint32_t>(-7));
    EXPECT_EQ(info.specializationConstants[2].type, RHIShaderSpecializationConstantType::eFloat);
    EXPECT_FLOAT_EQ(info.specializationConstants[2].floatValue, 1.25f);
}

TEST(ShaderReflectionTests, StorageQualifiersAndMemberAccessAreReflectedFromSPIRV)
{
    HeapVector<uint8_t> bytes = LoadReflectionFixture("rdg_access.comp.spv");
    ASSERT_FALSE(bytes.empty());

    RefCountPtr<zen::RHIShaderGroupSPIRV> spirv = MakeRefCountPtr<RHIShaderGroupSPIRV>();
    spirv->SetStageSPIRV(RHIShaderStage::eCompute, std::move(bytes));
    RHIShaderGroupInfo info;
    RHIShaderUtil::ReflectShaderGroupInfo(spirv, info);
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
        RHIShaderUtil::ReflectShaderGroupInfo(spirv, info);
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
            MergeOrAddSRDs(i == 0 ? RHIShaderStage::eVertex : RHIShaderStage::eFragment,
                           descriptors[i], info);
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
    RHIShaderUtil::ReflectShaderGroupInfo(both, info);
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
    for (const char* path :
         {"VoxelGI/reset_compute_indirect.comp.spv", "VoxelGI/reset_draw_indirect.comp.spv"})
    {
        ExpectAccess(ReflectStage(RHIShaderStage::eCompute, path), "IndirectBuffer", false, true);
    }

    const RHIShaderGroupInfo preDraw =
        ReflectStage(RHIShaderStage::eCompute, "VoxelGI/voxel_pre_draw.comp.spv");
    ExpectAccess(preDraw, "InstancePositionBuffer", false, true);
    ExpectAccess(preDraw, "InstanceColorBuffer", false, true);
    ExpectAccess(preDraw, "IndirectBuffer", true, true);
    ExpectAccess(ReflectStage(RHIShaderStage::eCompute, "VoxelGI/reset_voxel_texture.comp.spv"),
                 "voxelTexture", false, true);
}

TEST(ShaderReflectionTests, GeometryVoxelizationKeepsTheFullSceneVertexStride)
{
    const RHIShaderGroupInfo voxel =
        ReflectStage(RHIShaderStage::eVertex, "VoxelGI/voxelization.vert.spv");
    const RHIShaderGroupInfo gbuffer =
        ReflectStage(RHIShaderStage::eVertex, "SceneRenderer/offscreen.vert.spv");
    EXPECT_EQ(voxel.vertexBindingStride, sizeof(asset::Vertex));
    EXPECT_EQ(gbuffer.vertexBindingStride, sizeof(asset::Vertex));
    EXPECT_EQ(voxel.pushConstants.size, 4u * sizeof(uint32_t));
}

TEST(ShaderReflectionTests, SceneShadowsKeepTheMeshVertexLayoutAndUniformContract)
{
    const RHIShaderGroupInfo shadow =
        ReflectStage(RHIShaderStage::eVertex, "ShadowMapping/scene_shadow.vert.spv");
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
    for (const char* path : {"VoxelGI/clear_owners.comp.spv", "VoxelGI/resolve_albedo.comp.spv",
                             "VoxelGI/resolve_surface.comp.spv", "VoxelGI/sky_irradiance.comp.spv",
                             "VoxelGI/inject_radiance.comp.spv", "VoxelGI/filter_albedo.comp.spv",
                             "VoxelGI/filter_radiance.comp.spv", "VoxelGI/voxel_pre_draw.comp.spv"})
    {
        SCOPED_TRACE(path);
        const HeapVector<uint8_t> bytes = platform::FileSystem::LoadSpvFile(path);
        ASSERT_FALSE(bytes.empty());
        SpvReflectShaderModule module{};
        ASSERT_EQ(spvReflectCreateShaderModule(bytes.size(), bytes.data(), &module),
                  SPV_REFLECT_RESULT_SUCCESS);
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
            EXPECT_LE(uint64_t(entry->local_size.x) * entry->local_size.y * entry->local_size.z,
                      128u);
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
            EXPECT_EQ(constant.intValue, ZEN_VOXEL_VOLUME_GROUP_SIZE);
        }
        EXPECT_TRUE(seen[ZEN_VOXEL_VOLUME_GROUP_X_ID]);
        EXPECT_TRUE(seen[ZEN_VOXEL_VOLUME_GROUP_Y_ID]);
        EXPECT_TRUE(seen[ZEN_VOXEL_VOLUME_GROUP_Z_ID]);
    }
}
