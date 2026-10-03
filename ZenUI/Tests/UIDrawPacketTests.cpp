#include "UI/UIDrawPacket.h"
#include "Graphics/RHI/RHIShaderUtil.h"
#include "Platform/FileSystem.h"
#include <gtest/gtest.h>
#include <cmath>
#include <cstring>
#include <limits>

namespace zen::ui
{
namespace
{
TEST(UIShaderReflectionTest, PackedVertexColorHasFourByteStrideAndLocalFontBinding)
{
    RHIShaderGroupSPIRVPtr spirv = MakeRefCountPtr<RHIShaderGroupSPIRV>();

    spirv->SetStageSPIRV(RHIShaderStage::eVertex, platform::FileSystem::LoadSpvFile("UI/imgui.vert.spv"));

    spirv->SetStageSPIRV(RHIShaderStage::eFragment, platform::FileSystem::LoadSpvFile("UI/imgui.frag.spv"));

    RHIShaderGroupInfo info;

    ASSERT_TRUE(RHIShaderUtil::ReflectShaderGroupInfo(spirv, info));

    ASSERT_EQ(info.vertexInputAttributes.size(), 3u);

    EXPECT_EQ(info.vertexBindingStride, sizeof(ImDrawVert));

    EXPECT_EQ(info.vertexInputAttributes[2].offset, offsetof(ImDrawVert, col));

    EXPECT_EQ(info.vertexInputAttributes[2].format, DataFormat::eR32UInt);

    ASSERT_EQ(info.SRDTable.size(), 2u);

    EXPECT_TRUE(info.SRDTable[0].empty());

    ASSERT_EQ(info.SRDTable[1].size(), 1u);

    EXPECT_EQ(info.SRDTable[1][0].name, NameID("uFont"));

    EXPECT_EQ(info.pushConstants.size, 16u);
}

class UIDrawPacketTest : public testing::Test
{
protected:
    void SetUp() override
    {
        context                    = ImGui::CreateContext();

        ImGui::GetIO().IniFilename = nullptr;

        first                      = new ImDrawList(ImGui::GetDrawListSharedData());

        second                     = new ImDrawList(ImGui::GetDrawListSharedData());

        FillList(*first);

        FillList(*second);

        data.Valid            = true;

        data.DisplayPos       = ImVec2(100, 50);

        data.DisplaySize      = ImVec2(100, 80);

        data.FramebufferScale = ImVec2(1.5f, 2.0f);

        data.CmdLists.push_back(first);

        data.CmdLists.push_back(second);

        data.CmdListsCount = 2;

        data.TotalVtxCount = 6;

        data.TotalIdxCount = 6;
    }

    void TearDown() override
    {
        delete first;

        delete second;

        ImGui::DestroyContext(context);
    }

    void FillList(ImDrawList& list)
    {
        list.VtxBuffer.resize(3);

        list.VtxBuffer[0] = {ImVec2(10, 20), ImVec2(0, 0), 0xff001122};

        list.IdxBuffer.push_back(0);

        list.IdxBuffer.push_back(1);

        list.IdxBuffer.push_back(2);

        ImDrawCmd command;

        command.ClipRect  = ImVec4(99, 55.25f, 180.5f, 140);

        command.TexRef    = ImTextureRef(fontId);

        command.ElemCount = 3;

        list.CmdBuffer.push_back(command);
    }

    static constexpr ImTextureID fontId = 7;
    ImGuiContext*                context{nullptr};
    ImDrawList*                  first{nullptr};
    ImDrawList*                  second{nullptr};
    ImDrawData                   data;
};

TEST_F(UIDrawPacketTest, ScalesOffsetsAndClampsClipRectangle)
{
    UIDrawPacket packet;

    ASSERT_TRUE(BuildUIDrawPacket(data, fontId, 150, 160, packet));

    ASSERT_EQ(packet.commands.size(), 2u);

    EXPECT_EQ(packet.commands[0].minX, 0u);

    EXPECT_EQ(packet.commands[0].minY, 10u);

    EXPECT_EQ(packet.commands[0].maxX, 121u);

    EXPECT_EQ(packet.commands[0].maxY, 160u);

    EXPECT_FLOAT_EQ(packet.projection[0], 0.02f);

    EXPECT_FLOAT_EQ(packet.projection[3], -2.25f);

    EXPECT_EQ(packet.commands[1].firstIndex, 3u);

    EXPECT_EQ(packet.commands[1].vertexOffset, 3u);
}

TEST_F(UIDrawPacketTest, SnapshotOwnsGeometryAfterSourceMutationAndPadsIndexUpload)
{
    data.CmdLists.pop_back();

    data.CmdListsCount = 1;

    data.TotalVtxCount = data.TotalIdxCount = 3;

    UIDrawPacket packet;

    ASSERT_TRUE(BuildUIDrawPacket(data, fontId, 150, 160, packet));

    EXPECT_EQ(packet.indices.size() % 4, 0u);

    first->VtxBuffer[0].pos.x = 500;

    first->IdxBuffer[0]       = 2;

    ImDrawVert vertex;

    std::memcpy(&vertex, packet.vertices.data(), sizeof(vertex));

    EXPECT_FLOAT_EQ(vertex.pos.x, 10);

    EXPECT_EQ(packet.indices[0], 0u);
}

TEST_F(UIDrawPacketTest, PreservesBaseVertexBeyondSixteenBitIndexRange)
{
    first->VtxBuffer.resize(70003);

    data.TotalVtxCount            = 70006;

    first->CmdBuffer[0].VtxOffset = 70000;

    UIDrawPacket packet;

    ASSERT_TRUE(BuildUIDrawPacket(data, fontId, 150, 160, packet));

    EXPECT_EQ(packet.commands[0].vertexOffset, 70000u);

    EXPECT_EQ(packet.commands[1].vertexOffset, 70003u);
}

TEST_F(UIDrawPacketTest, RejectsUnknownTextureAndClearsPartialPacket)
{
    second->CmdBuffer[0].TexRef = ImTextureRef(ImTextureID(999));

    UIDrawPacket packet;

    EXPECT_FALSE(BuildUIDrawPacket(data, fontId, 150, 160, packet));

    EXPECT_TRUE(packet.commands.empty());

    EXPECT_TRUE(packet.vertices.empty());
}

void UnsupportedCallback(const ImDrawList*, const ImDrawCmd*) {}

TEST_F(UIDrawPacketTest, AcceptsResetCallbackAndRejectsArbitraryCallbacks)
{
    second->CmdBuffer[0].UserCallback = ImDrawCallback_ResetRenderState;

    UIDrawPacket packet;

    EXPECT_TRUE(BuildUIDrawPacket(data, fontId, 150, 160, packet));

    EXPECT_EQ(packet.commands.size(), 1u);

    second->CmdBuffer[0].UserCallback = UnsupportedCallback;

    EXPECT_FALSE(BuildUIDrawPacket(data, fontId, 150, 160, packet));
}

TEST_F(UIDrawPacketTest, EmptyMinimizedAndClippedFramesProduceNoDraws)
{
    UIDrawPacket packet;

    EXPECT_TRUE(BuildUIDrawPacket(data, fontId, 0, 0, packet));

    EXPECT_TRUE(packet.commands.empty());

    for (ImDrawList* list : data.CmdLists)
    {
        list->CmdBuffer[0].ClipRect = ImVec4(-20, -20, -10, -10);
    }

    EXPECT_TRUE(BuildUIDrawPacket(data, fontId, 150, 160, packet));

    EXPECT_TRUE(packet.commands.empty());

    data.Clear();

    data.Valid            = true;

    data.DisplaySize      = ImVec2(150, 160);

    data.FramebufferScale = ImVec2(1, 1);

    EXPECT_TRUE(BuildUIDrawPacket(data, fontId, 150, 160, packet));

    EXPECT_TRUE(packet.commands.empty());
}

TEST_F(UIDrawPacketTest, RejectsInvalidRangesAndNonFiniteClipping)
{
    UIDrawPacket packet;

    first->CmdBuffer[0].IdxOffset = 3;

    EXPECT_FALSE(BuildUIDrawPacket(data, fontId, 150, 160, packet));

    first->CmdBuffer[0].IdxOffset  = 0;

    first->CmdBuffer[0].ClipRect.x = std::numeric_limits<float>::quiet_NaN();

    EXPECT_FALSE(BuildUIDrawPacket(data, fontId, 150, 160, packet));
}
} // namespace
} // namespace zen::ui
