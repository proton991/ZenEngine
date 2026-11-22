#include "ZenRHI/RHIShaderUtil.h"
#include "Platform/FileSystem.h"
#include <gtest/gtest.h>
#include <imgui.h>

using namespace zen;

TEST(UIRenderingContract, PackedVertexColorHasFourByteStrideAndTextureSlotArrays)
{
    RHIShaderGroupSPIRVPtr spirv = MakeRefCountPtr<RHIShaderGroupSPIRV>();

    spirv->SetStageSPIRV(RHIShaderStage::eVertex, platform::FileSystem::LoadSpvFile("UI/ui.vert.spv"));

    spirv->SetStageSPIRV(RHIShaderStage::eFragment, platform::FileSystem::LoadSpvFile("UI/ui.frag.spv"));

    RHIShaderGroupInfo info;

    // Vulkan's guaranteed descriptor set minimum: the shader must load on every device.
    ASSERT_TRUE(RHIShaderUtil::ReflectShaderGroupInfo(spirv, RHIGPUInfo{}.maxBoundDescriptorSets, info));

    ASSERT_EQ(info.vertexInputAttributes.size(), 3u);

    EXPECT_EQ(info.vertexBindingStride, sizeof(ImDrawVert));

    EXPECT_EQ(info.vertexInputAttributes[2].offset, offsetof(ImDrawVert, col));

    EXPECT_EQ(info.vertexInputAttributes[2].format, DataFormat::eR32UInt);

    ASSERT_EQ(info.SRDTable.size(), 2u);

    EXPECT_TRUE(info.SRDTable[0].empty());

    ASSERT_EQ(info.SRDTable[1].size(), 2u);

    EXPECT_EQ(info.SRDTable[1][0].name, "uTextures");

    EXPECT_EQ(info.SRDTable[1][0].arraySize, 16u);

    EXPECT_EQ(info.SRDTable[1][1].name, "uSamplers");

    EXPECT_EQ(info.SRDTable[1][1].arraySize, 16u);

    // Projection followed by the per-draw texture slot.
    EXPECT_EQ(info.pushConstants.size, 20u);
}
