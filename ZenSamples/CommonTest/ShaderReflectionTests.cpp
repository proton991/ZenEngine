#include "Graphics/RHI/RHIShaderUtil.h"
#include "Platform/FileSystem.h"
#include <gtest/gtest.h>

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
    ASSERT_EQ(info.SRDTable[0].size(), 3u);
    ASSERT_EQ(info.SRDTable[1].size(), 1u);

    for (uint32_t binding = 0; binding < 3; ++binding)
    {
        EXPECT_EQ(info.SRDTable[0][binding].set, 0u);
        EXPECT_EQ(info.SRDTable[0][binding].binding, binding);
    }

    EXPECT_EQ(info.SRDTable[1][0].set, 1u);
    EXPECT_EQ(info.SRDTable[1][0].binding, 0u);
    EXPECT_EQ(info.SRDTable[1][0].arraySize, 1024u);
}

TEST(ShaderReflectionTests, VoxelShaderInitializesAllDescriptorSets)
{
    RefCountPtr<zen::RHIShaderGroupSPIRV> spirv = MakeRefCountPtr<RHIShaderGroupSPIRV>();
    spirv->SetStageSPIRV(RHIShaderStage::eCompute,
                         platform::FileSystem::LoadSpvFile("VoxelGI/voxelization.comp.spv"));

    RHIShaderGroupInfo info{};
    RHIShaderUtil::ReflectShaderGroupInfo(spirv, info);

    const uint32_t bindingCounts[] = {1, 1, 3, 1, 2, 1};
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

    ASSERT_EQ(info.SRDTable.size(), 3u);
    EXPECT_TRUE(info.SRDTable[0].empty());
    EXPECT_TRUE(info.SRDTable[1].empty());
    ASSERT_EQ(info.SRDTable[2].size(), 1u);
    EXPECT_EQ(info.SRDTable[2][0].set, 2u);
    EXPECT_EQ(info.SRDTable[2][0].binding, 0u);
    EXPECT_EQ(info.SRDTable[2][0].type, RHIShaderResourceType::eStorageBuffer);
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
    const RHIShaderGroupInfo consumer =
        ReflectStage(RHIShaderStage::eCompute, "VoxelGI/voxelization_large_triangles.comp.spv");

    for (const zen::RHIShaderGroupInfo* shader : {&producer, &consumer})
    {
        for (NameID name : {"VertexBuffer", "IndexBuffer", "NodeBuffer", "TriangleMap"})
        {
            ExpectWritable(*shader, name, false);
        }

        ExpectWritable(*shader, "voxelTexture", true);
    }

    ExpectWritable(producer, "LargeTriangleArray", true);
    ExpectWritable(producer, "IndirectBuffer", true);
    ExpectWritable(consumer, "LargeTriangleArray", false);

    ASSERT_GT(consumer.SRDTable.size(), 4u);
    ASSERT_EQ(consumer.SRDTable[4].size(), 1u);
    EXPECT_EQ(consumer.SRDTable[4][0].binding, 1u);
    EXPECT_EQ(FindBinding(consumer, "IndirectBuffer"), nullptr);
}

TEST(ShaderReflectionTests, VoxelAtomicAndRadianceWritesRemainWritable)
{
    const RHIShaderGroupInfo geometry =
        ReflectStage(RHIShaderStage::eFragment, "VoxelGI/voxelization.frag.spv");
    ExpectWritable(geometry, "voxelAlbedo", true);

    for (NameID name : {"voxelNormal", "voxelEmissive", "staticVoxelFlag"})
    {
        EXPECT_EQ(FindBinding(geometry, name), nullptr);
    }

    const RHIShaderGroupInfo inject =
        ReflectStage(RHIShaderStage::eCompute, "VoxelGI/inject_radiance.comp.spv");
    ExpectWritable(inject, "voxelNormal", true); // Reads normals, then writes occupancy into alpha.
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
