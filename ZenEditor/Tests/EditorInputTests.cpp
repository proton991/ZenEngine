#include "Editor/ImGui/EditorTheme.h"
#include "EditorWidgets.h"
#include "imgui.h"
#include <gtest/gtest.h>

namespace zen::editor
{
namespace
{
// Exercises the frontend's actual ImGui configuration without a native window or GPU.
class EditorInput : public testing::Test
{
protected:
    void SetUp() override
    {
        ImGui::CreateContext();

        ImGuiIO& io    = ImGui::GetIO();

        io.IniFilename = nullptr;

        io.DisplaySize = ImVec2(1000, 800);

        io.DeltaTime   = 1.0f / 60.0f;

        ApplyEditorTheme(1.0f);

        unsigned char* pixels = nullptr;

        int width             = 0;

        int height            = 0;

        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

        io.Fonts->SetTexID(1);

        DrawFrame();

        DrawFrame();
    }

    void TearDown() override
    {
        ImGui::DestroyContext();
    }

    void DrawFrame()
    {
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(100, 100), ImGuiCond_Once);

        ImGui::SetNextWindowSize(ImVec2(400, 400), ImGuiCond_Once);

        ImGui::Begin("Floating Scene");

        ImGui::SetWindowFocus();

        allowed     = DrawSceneImage(1, ImVec2(300, 300), focused);

        position    = ImGui::GetWindowPos();

        imageOrigin = ImGui::GetItemRectMin();

        active      = ImGui::IsAnyItemActive();

        ImGui::End();

        ImGui::Render();
    }

    ImVec2 position;
    ImVec2 imageOrigin;
    bool   active{false};
    bool   allowed{false};
    bool   focused{true};
};

TEST_F(EditorInput, AltDragOnSceneImageLeavesNavigationInputAvailable)
{
    ImGuiIO& io        = ImGui::GetIO();

    const ImVec2 start = position;

    const ImVec2 mouse(imageOrigin.x + 40, imageOrigin.y + 40);

    io.AddMousePosEvent(mouse.x, mouse.y);

    io.AddKeyEvent(ImGuiMod_Alt, true);

    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);

    DrawFrame();

    io.AddMousePosEvent(mouse.x + 40, mouse.y + 20);

    DrawFrame();

    EXPECT_EQ(position.x, start.x);

    EXPECT_EQ(position.y, start.y);

    EXPECT_TRUE(active);

    EXPECT_TRUE(allowed);

    EXPECT_TRUE(io.KeyAlt);

    EXPECT_TRUE(ImGui::IsMouseDown(ImGuiMouseButton_Left));

    EXPECT_FLOAT_EQ(io.MouseDelta.x, 40.0f);

    EXPECT_FLOAT_EQ(io.MouseDelta.y, 20.0f);

    focused = false;

    DrawFrame();

    EXPECT_FALSE(allowed);
}

TEST_F(EditorInput, FloatingPanelsStillMoveByTheirTitleBar)
{
    ImGuiIO& io        = ImGui::GetIO();

    const ImVec2 start = position;

    const ImVec2 mouse(start.x + 180, start.y + 10);

    io.AddMousePosEvent(mouse.x, mouse.y);

    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);

    DrawFrame();

    io.AddMousePosEvent(mouse.x + 40, mouse.y + 20);

    DrawFrame();

    EXPECT_FLOAT_EQ(position.x, start.x + 40);

    EXPECT_FLOAT_EQ(position.y, start.y + 20);
}
} // namespace
} // namespace zen::editor
