#include "Editor/ImGui/EditorTheme.h"
#include "EditorWidgets.h"
#include "EditorShortcuts.h"
#include "SceneViewOverlays.h"
#include "Editor/Model/ViewAxes.h"
#include <cmath>
#include "imgui.h"
#include "imgui_internal.h"
#include <gtest/gtest.h>

namespace zen::editor
{
namespace
{
void DoNothing() {}

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

        HandleActionShortcuts(shortcutActions, EditorShortcutScope::Global, focused, nativeMenuShortcuts);

        ImGui::SetNextWindowPos(ImVec2(100, 100), ImGuiCond_Once);

        ImGui::SetNextWindowSize(ImVec2(400, 400), ImGuiCond_Once);

        ImGui::Begin("Floating Scene");

        ImGui::SetWindowFocus();

        allowed            = DrawSceneImage(1, ImVec2(300, 300), focused);

        const bool hovered = ImGui::IsItemHovered();

        position           = ImGui::GetWindowPos();

        imageOrigin        = ImGui::GetItemRectMin();

        imageEnd           = ImGui::GetItemRectMax();

        active             = ImGui::IsAnyItemActive();

        if (drawAxes)
        {
            hover = DrawViewAxes(camera.GetCamera().GetViewMatrix(), imageOrigin, imageEnd,
                                 allowed && hovered && !sphere.IsActive(), sphere.IsActive());

            sphere.Update(camera, hover, allowed && hovered);
        }

        ImGui::End();

        ImGui::Render();
    }

    ImVec2        position;
    ImVec2        imageOrigin;
    ImVec2        imageEnd;
    bool          active{false};
    bool          allowed{false};
    bool          focused{true};
    bool          drawAxes{false};
    bool          nativeMenuShortcuts{false};
    EditorActions shortcutActions;
    // The Scene panel's sphere wiring, with a standalone camera.
    EditorCamera    camera;
    ViewSphereInput sphere;
    ViewAxesHover   hover;
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

TEST_F(EditorInput, NativeMenuShortcutsDoNotExecuteAgainThroughImGui)
{
    int opened = 0;

    int framed = 0;

    shortcutActions.Register(
        {actions::Open, "Open...", {platform::Key::O, uint16_t(platform::KeyModifier::Super)}, "", nullptr, [&opened]() {
             ++opened;
         }});

    shortcutActions.Register(
        {actions::FrameAll, "Frame All", {platform::Key::Home, 0}, "", nullptr, [&framed]() { ++framed; }});

    nativeMenuShortcuts = true;

    ImGuiIO& io         = ImGui::GetIO();

    // SDL forwards this key to ImGui even when AppKit dispatches the menu item.
    io.AddKeyEvent(ImGuiMod_Super, true);

    io.AddKeyEvent(ImGuiKey_O, true);

    shortcutActions.Execute(actions::Open);

    DrawFrame();

    EXPECT_EQ(opened, 1);

    io.AddKeyEvent(ImGuiKey_O, false);

    io.AddKeyEvent(ImGuiMod_Super, false);

    DrawFrame();

    // Plain scene shortcuts still belong to the workspace.
    io.AddKeyEvent(ImGuiKey_Home, true);

    DrawFrame();

    EXPECT_EQ(framed, 1);
}

TEST_F(EditorInput, ViewSphereReportsTheSphereAndAxisEndUnderTheMouse)
{
    ImGuiIO& io = ImGui::GetIO();

    // The default editor camera looks down -Z.
    const Mat4 view = camera.GetCamera().GetViewMatrix();

    drawAxes        = true;

    ImVec2 end;

    ASSERT_TRUE(GetViewAxisPosition(view, imageOrigin, imageEnd, Vec3(1, 0, 0), end));

    EXPECT_GT(end.x, imageOrigin.x + 150);

    EXPECT_LT(end.y, imageOrigin.y + 150);

    io.AddMousePosEvent(end.x, end.y);

    DrawFrame();

    EXPECT_TRUE(hover.sphere);

    EXPECT_EQ(hover.axis, Vec3(1, 0, 0));

    ImVec2 center;

    float radius = 0;

    ASSERT_TRUE(GetViewSphereCenter(imageOrigin, imageEnd, center, radius));

    io.AddMousePosEvent(center.x + radius * 0.5f, center.y + radius * 0.5f);

    DrawFrame();

    EXPECT_TRUE(hover.sphere);

    EXPECT_EQ(hover.axis, Vec3(0.0f));

    // A press outside the sphere leaves the left button to picking.
    io.AddMousePosEvent(imageOrigin.x + 40, imageOrigin.y + 260);

    DrawFrame();

    EXPECT_FALSE(hover.sphere);

    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);

    DrawFrame();

    EXPECT_FALSE(sphere.IsActive());

    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);

    DrawFrame();

    // Without window focus the sphere still draws but does not respond.
    io.AddMousePosEvent(end.x, end.y);

    focused = false;

    DrawFrame();

    EXPECT_FALSE(hover.sphere);

    // Small images leave the scene unobstructed.
    EXPECT_FALSE(GetViewAxisPosition(view, imageOrigin, ImVec2(imageOrigin.x + 120, imageOrigin.y + 120), Vec3(1, 0, 0), end));
}

TEST_F(EditorInput, DraggingTheSphereOrbitsTheCameraAndClickingAnAxisViewsFromIt)
{
    ImGuiIO& io = ImGui::GetIO();

    drawAxes    = true;

    DrawFrame();

    ImVec2 center;

    float radius = 0;

    ASSERT_TRUE(GetViewSphereCenter(imageOrigin, imageEnd, center, radius));

    // Grab the sphere away from its axis ends and drag right by half its radius.
    const ImVec2 grab(center.x + radius * 0.5f, center.y + radius * 0.5f);

    io.AddMousePosEvent(grab.x, grab.y);

    DrawFrame();

    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);

    DrawFrame();

    EXPECT_TRUE(sphere.IsActive());

    io.AddMousePosEvent(grab.x + radius * 0.5f, grab.y);

    DrawFrame();

    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);

    DrawFrame();

    EXPECT_FALSE(sphere.IsActive());

    // The view turned half a radian; the sphere's front (+Z) followed the cursor right.
    Mat4 view = camera.GetCamera().GetViewMatrix();

    EXPECT_NEAR(ToViewAxesSpace(view, Vec3(0, 0, 1)).x, std::sin(0.5f), 1e-3f);

    EXPECT_NEAR(ToViewAxesSpace(view, Vec3(0, 1, 0)).y, -1.0f, 1e-4f);

    // A click on an axis end, without dragging, views the scene from that side.
    ImVec2 end;

    ASSERT_TRUE(GetViewAxisPosition(view, imageOrigin, imageEnd, Vec3(1, 0, 0), end));

    io.AddMousePosEvent(end.x, end.y);

    DrawFrame();

    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);

    DrawFrame();

    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);

    DrawFrame();

    view = camera.GetCamera().GetViewMatrix();

    EXPECT_GT(ToViewAxesSpace(view, Vec3(1, 0, 0)).z, 0.999f);
}

TEST_F(EditorInput, ControlsHintDrawsOnlyWhenTheImageCanHoldIt)
{
    EditorActions registry;

    registry.Register({actions::FrameAll, "Frame All", {platform::Key::Home, 0}, "", nullptr, &DoNothing});

    ImGui::NewFrame();

    ImGui::Begin("Floating Scene");

    ImDrawList& draw  = *ImGui::GetWindowDrawList();

    const int initial = draw.VtxBuffer.Size;

    DrawSceneControlsHint(registry, ImVec2(0, 0), ImVec2(120, 120));

    const int afterTiny = draw.VtxBuffer.Size;

    DrawSceneControlsHint(registry, ImVec2(0, 0), ImVec2(900, 700));

    const int afterRoomy = draw.VtxBuffer.Size;

    ImGui::End();

    ImGui::Render();

    EXPECT_EQ(afterTiny, initial);

    EXPECT_GT(afterRoomy, afterTiny);
}

TEST_F(EditorInput, ActionTooltipsNameTheCommandAndWhyItIsDisabled)
{
    const EditorAction open{.id             = "open",
                            .label          = "Open...",
                            .shortcut       = {platform::Key::O, uint16_t(platform::KeyModifier::Control)},
                            .disabledReason = "Busy"};

    const std::string shortcut = FormatShortcut(open.shortcut);

    ASSERT_FALSE(shortcut.empty());

    // Icon-only toolbar buttons rely on the tooltip for their name, even while disabled.
    EXPECT_EQ(FormatActionTooltip(open, true), "Open (" + shortcut + ")");

    EXPECT_EQ(FormatActionTooltip(open, false), "Open (" + shortcut + ")\nBusy");

    const EditorAction run{.id = "run", .label = "Run"};

    EXPECT_EQ(FormatActionTooltip(run, false), "Run");
}

TEST_F(EditorInput, PropertyRowsStackLabelsOnlyInNarrowPanels)
{
    for (const float width : {420.0f, 180.0f})
    {
        SCOPED_TRACE(width);

        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));

        ImGui::SetNextWindowSize(ImVec2(width, 300));

        ImGui::Begin("Properties");

        const ImVec2 row        = ImGui::GetCursorScreenPos();

        const std::string label = PropertyLabel("Intensity");

        float value             = 1.0f;

        ImGui::DragFloat(label.c_str(), &value);

        const ImVec2 widget      = ImGui::GetItemRectMin();

        const ImVec2 widgetEnd   = ImGui::GetItemRectMax();

        const float contentRight = row.x + ImGui::GetContentRegionAvail().x;

        ImGui::End();

        ImGui::Render();

        // The widget ID ignores the layout, so resizing keeps its state.
        EXPECT_EQ(label, "###Intensity");

        if (width > 300)
        {
            EXPECT_FLOAT_EQ(widget.y, row.y);

            EXPECT_GT(widget.x, row.x);
        }
        else
        {
            EXPECT_GT(widget.y, row.y);

            EXPECT_FLOAT_EQ(widget.x, row.x);
        }

        EXPECT_LE(widgetEnd.x, contentRight + 0.5f);
    }
}

TEST_F(EditorInput, ClippedLabelsShowTheirTextInDisabledSections)
{
    const char* label = "A property label far too long for its narrow column";

    ImVec2 labelMin;

    for (int frame = 0; frame < 3; ++frame)
    {
        ImGui::GetIO().MousePos = ImVec2(labelMin.x + 4, labelMin.y + 4);

        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));

        ImGui::SetNextWindowSize(ImVec2(400, 200));

        ImGui::Begin("Disabled properties");

        ImGui::BeginDisabled();

        DrawClippedText(label, ImVec2(60, ImGui::GetFrameHeight()), ImGui::GetColorU32(ImGuiCol_Text));

        labelMin = ImGui::GetItemRectMin();

        ImGui::EndDisabled();

        ImGui::End();

        ImGui::Render();
    }

    const ImGuiWindow* tooltip = ImGui::FindWindowByName("##Tooltip_00");

    ASSERT_NE(tooltip, nullptr);

    EXPECT_TRUE(tooltip->Active);
}

TEST_F(EditorInput, LogLinesGiveEveryLineOfAnEntryItsOwnRow)
{
    HeapVector<EditorLogEntry> entries;

    entries.push_back({2, "single"});

    entries.push_back({4, "metrics\n  group=0\n  details"});

    entries.push_back({3, "trailing break\n"});

    entries.push_back({2, ""});

    const HeapVector<LogLine> lines = SplitLogLines(entries);

    ASSERT_EQ(lines.size(), 6u);

    const auto text = [&entries](const LogLine& line) {
        return entries[line.entry].text.substr(line.begin, line.end - line.begin);
    };

    EXPECT_EQ(text(lines[0]), "single");

    EXPECT_EQ(text(lines[1]), "metrics");

    EXPECT_EQ(text(lines[2]), "  group=0");

    EXPECT_EQ(text(lines[3]), "  details");

    EXPECT_EQ(lines[3].entry, 1u);

    EXPECT_EQ(text(lines[4]), "trailing break");

    // An empty entry still occupies a row.
    EXPECT_EQ(lines[5].entry, 3u);

    EXPECT_EQ(text(lines[5]), "");
}
} // namespace
} // namespace zen::editor
