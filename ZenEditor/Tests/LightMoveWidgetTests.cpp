#include "LightMoveWidget.h"
#include "EditorWidgets.h"
#include "Editor/ImGui/EditorTheme.h"
#include <gtest/gtest.h>

namespace zen::editor
{
namespace
{
class LightMove : public testing::Test
{
protected:
    void SetUp() override
    {
        ImGui::CreateContext();
        ImGuiIO& io    = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1200, 900);
        io.DeltaTime   = 1.0f / 60.0f;
        ApplyEditorTheme(1.0f);
        camera.SetExtent(400, 300);
        rc::RenderingLight light;
        light.id             = 7;
        light.light.type     = rc::SceneLightType::ePoint;
        light.light.position = Vec3(0);
        lights.push_back(light);
        DrawFrame();
        DrawFrame();
    }

    void TearDown() override
    {
        ImGui::DestroyContext();
    }

    void DrawFrame()
    {
        // Applying a new UI scale rebuilds the font atlas in the actual backend.
        unsigned char* pixels = nullptr;
        int            width  = 0;
        int            height = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        ImGui::GetIO().Fonts->SetTexID(1);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(100, 100));
        ImGui::SetNextWindowSize(ImVec2(600, 500));
        ImGui::Begin("Light movement");
        ImGui::SetWindowFocus();
        const bool allowed = DrawSceneImage(1, extent, focused);
        const bool hovered = ImGui::IsItemHovered();
        origin             = ImGui::GetItemRectMin();
        changed            = widget.Draw(lights, camera, generation, origin, extent, enabled, allowed, hovered && canStart);
        ImGui::End();
        ImGui::Render();
    }

    ImVec2 ScreenPosition(Vec3 position) const
    {
        const Vec4 clip = camera.GetCamera().GetProjectionMatrix() * camera.GetCamera().GetViewMatrix() * Vec4(position, 1);
        return ImVec2(origin.x + (clip.x / clip.w + 1) * extent.x * 0.5f, origin.y + (clip.y / clip.w + 1) * extent.y * 0.5f);
    }

    void Press(ImVec2 mouse)
    {
        ImGui::GetIO().AddMousePosEvent(mouse.x, mouse.y);
        DrawFrame();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        DrawFrame();
    }

    void Move(ImVec2 mouse)
    {
        ImGui::GetIO().AddMousePosEvent(mouse.x, mouse.y);
        DrawFrame();
    }

    void Release()
    {
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        DrawFrame();
    }

    HeapVector<rc::RenderingLight> lights;
    EditorCamera                   camera;
    LightMoveWidget                widget;
    ImVec2                         origin;
    ImVec2                         extent{400, 300};
    uint64_t                       generation{1};
    bool                           focused{true};
    bool                           enabled{true};
    bool                           canStart{true};
    bool                           changed{false};
};

TEST_F(LightMove, DragFollowsCursorAtConstantViewDepthWithoutSnappingInBothProjections)
{
    for (const bool orthographic : {false, true})
    {
        camera.SetOrthographic(orthographic);
        camera.LookAlong(Vec3(-1, -0.3f, -1));
        for (const float scale : {1.0f, 1.5f, 2.0f})
        {
            ApplyEditorTheme(scale);
            ImGui::GetIO().DisplayFramebufferScale = ImVec2(scale, scale);
            lights.front().light.position          = Vec3(0);
            DrawFrame();
            const ImVec2 start = ScreenPosition(Vec3(0));
            const ImVec2 grab(start.x + 4 * scale, start.y);
            const Vec4   viewStart = camera.GetCamera().GetViewMatrix() * Vec4(0, 0, 0, 1);
            Press(grab);
            ASSERT_TRUE(widget.IsActive());
            EXPECT_TRUE(widget.OwnsMouse());
            EXPECT_FALSE(changed);
            EXPECT_EQ(lights.front().light.position, Vec3(0));
            Move(ImVec2(grab.x + 45, grab.y + 30));
            EXPECT_TRUE(changed);
            const ImVec2 moved = ScreenPosition(lights.front().light.position);
            EXPECT_NEAR(moved.x, start.x + 45, 0.05f);
            EXPECT_NEAR(moved.y, start.y + 30, 0.05f);
            const Vec4 viewMoved = camera.GetCamera().GetViewMatrix() * Vec4(lights.front().light.position, 1);
            EXPECT_NEAR(viewStart.z, viewMoved.z, 1e-4f);
            const Vec3 position = lights.front().light.position;
            Release();
            EXPECT_FALSE(widget.IsActive());
            EXPECT_TRUE(widget.OwnsMouse());
            EXPECT_EQ(lights.front().light.position, position);
            DrawFrame();
            EXPECT_FALSE(widget.OwnsMouse());
        }
    }
}

TEST_F(LightMove, DragContinuesOutsideImageAndEscapeRestoresOnlyPosition)
{
    const ImVec2 start = ScreenPosition(Vec3(0));
    Press(start);
    ASSERT_TRUE(widget.IsActive());
    Move(ImVec2(origin.x + extent.x + 40, start.y));
    EXPECT_TRUE(widget.IsActive());
    EXPECT_TRUE(changed);
    EXPECT_GT(lights.front().light.position.x, 0);
    lights.front().light.intensity = 42;
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    DrawFrame();
    EXPECT_TRUE(changed);
    EXPECT_FALSE(widget.IsActive());
    EXPECT_TRUE(widget.OwnsMouse());
    EXPECT_EQ(lights.front().light.position, Vec3(0));
    EXPECT_EQ(lights.front().light.intensity, 42);
    Release();
    EXPECT_EQ(lights.front().light.position, Vec3(0));
}

TEST_F(LightMove, ReleaseUsesTheFinalMousePositionAndClickAloneDoesNotMove)
{
    const ImVec2 start = ScreenPosition(Vec3(0));
    Press(start);
    Release();
    EXPECT_FALSE(changed);
    EXPECT_EQ(lights.front().light.position, Vec3(0));
    Press(start);
    Move(ImVec2(start.x + 20, start.y));
    ImGui::GetIO().AddMousePosEvent(start.x + 50, start.y);
    Release();
    EXPECT_FALSE(widget.IsActive());
    EXPECT_TRUE(widget.OwnsMouse());
    EXPECT_NEAR(ScreenPosition(lights.front().light.position).x, start.x + 50, 0.05f);
}

TEST_F(LightMove, FocusLossModeExitAndCameraChangesCancel)
{
    for (int reason = 0; reason < 3; ++reason)
    {
        focused = enabled = true;
        DrawFrame();
        const ImVec2 start = ScreenPosition(Vec3(0));
        Press(start);
        Move(ImVec2(start.x + 40, start.y));
        ASSERT_TRUE(changed);
        if (reason == 0)
        {
            focused = false;
        }
        else if (reason == 1)
        {
            enabled = false;
        }
        else
        {
            camera.SetOrthographic(true);
        }
        DrawFrame();
        EXPECT_FALSE(widget.IsActive());
        EXPECT_TRUE(changed);
        EXPECT_EQ(lights.front().light.position, Vec3(0));
        Release();
    }
}

TEST_F(LightMove, SceneReplacementRemovalAndExternalPositionEditsAreNotOverwritten)
{
    for (int reason = 0; reason < 3; ++reason)
    {
        lights.resize(1);
        lights.front().id             = 7;
        lights.front().light.type     = rc::SceneLightType::ePoint;
        lights.front().light.position = Vec3(0);
        const ImVec2 start            = ScreenPosition(Vec3(0));
        Press(start);
        Move(ImVec2(start.x + 40, start.y));
        ASSERT_TRUE(changed);
        const Vec3 moved = lights.front().light.position;
        if (reason == 0)
        {
            ++generation;
        }
        else if (reason == 1)
        {
            lights.clear();
        }
        else
        {
            lights.front().light.position = Vec3(1);
        }
        DrawFrame();
        EXPECT_FALSE(widget.IsActive());
        EXPECT_FALSE(changed);
        if (!lights.empty())
        {
            EXPECT_EQ(lights.front().light.position, reason == 0 ? moved : Vec3(1));
        }
        Release();
    }
}

TEST_F(LightMove, OverlappingHandlesChooseNearestAndKeepStableIdentityAfterReordering)
{
    rc::RenderingLight front = lights.front();
    front.id                 = 9;
    front.light.position.z   = 0.5f;
    front.light.type         = rc::SceneLightType::eSpot;
    front.light.enabled      = false;
    lights.push_back(front);
    const ImVec2 start = ScreenPosition(front.light.position);
    Press(start);
    ASSERT_TRUE(widget.IsActive());
    std::swap(lights.front(), lights.back());
    Move(ImVec2(start.x + 40, start.y));
    EXPECT_TRUE(changed);
    EXPECT_GT(lights.front().light.position.x, 0);
    EXPECT_EQ(lights.back().light.position, Vec3(0));
    Release();
}

TEST_F(LightMove, CameraGesturesBlockedInputAndNonPositionalLightsDoNotStartDrags)
{
    for (int reason = 0; reason < 7; ++reason)
    {
        lights.front().light.position = reason == 5 ? Vec3(0, 0, 3) : Vec3(0);
        lights.front().light.type     = reason == 4 ? rc::SceneLightType::eDirectional : rc::SceneLightType::ePoint;
        enabled                       = reason != 0;
        focused                       = reason != 1;
        canStart                      = reason != 2;
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, reason == 3);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, reason == 6);
        Press(ScreenPosition(Vec3(0)));
        EXPECT_FALSE(widget.IsActive());
        EXPECT_FALSE(widget.OwnsMouse());
        EXPECT_FALSE(changed);
        Release();
    }
}
} // namespace
} // namespace zen::editor
