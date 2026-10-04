#include "InspectorNavigationWidget.h"
#include "EditorShortcuts.h"
#include "Editor/ImGui/EditorTheme.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <gtest/gtest.h>

namespace zen::editor
{
namespace
{
class InspectorControls : public testing::Test
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

        std::string error;

        UniquePtr<LoadedScene> loaded = ParseScene(std::string(ZEN_EDITOR_FIXTURES) + "textured.gltf", error);

        ASSERT_TRUE(loaded) << error;

        scene.Replace(std::move(loaded));

        navigation.RegisterActions(registry);

        for (const SceneAssetItem& item : scene.GetAssets().Query(""))
        {
            if (item.id.kind == SceneAssetKind::Material)
            {
                material = {{}, item.id};
            }
            else if (item.id.kind == SceneAssetKind::Texture)
            {
                texture = {{}, item.id};
            }
        }

        node = {scene.GetAssets().Inspect(material.asset).nodes[0], {}};

        selection.SelectNode(node.node);

        DrawFrame();

        navigation.Open(material);

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

        // The workspace dispatches global commands before individual panels.
        HandleActionShortcuts(registry, EditorShortcutScope::Global, focused);

        if (!inspectorFocused)
        {
            ImGui::Begin("Another panel");

            ImGui::SetWindowFocus();

            ImGui::End();
        }

        ImGui::SetNextWindowPos(ImVec2(100, 100), ImGuiCond_Once);

        ImGui::SetNextWindowSize(ImVec2(600, 600), ImGuiCond_Once);

        ImGui::Begin("Inspector controls");

        if (inspectorFocused)
        {
            ImGui::SetWindowFocus();
        }

        widget.Draw(navigation, registry, scene, focused);

        ImGui::End();

        ImGui::Render();
    }

    void AltArrow(ImGuiKey key)
    {
        ImGuiIO& io = ImGui::GetIO();

        io.AddKeyEvent(ImGuiMod_Alt, true);

        io.AddKeyEvent(key, true);

        DrawFrame();

        io.AddKeyEvent(key, false);

        io.AddKeyEvent(ImGuiMod_Alt, false);

        DrawFrame();
    }

    void ClickTab(int index, ImGuiMouseButton button)
    {
        // Read actual layout geometry; inject ordinary mouse events into the real widget.
        ImGuiWindow* window = ImGui::FindWindowByName("Inspector controls");

        ASSERT_NE(window, nullptr);

        ImGuiTabBar* bar = ImGui::GetCurrentContext()->TabBars.GetByKey(window->GetID("InspectorTabs"));

        ASSERT_NE(bar, nullptr);

        ASSERT_LT(index, bar->Tabs.Size);

        const ImGuiTabItem& tab = bar->Tabs[index];

        const ImVec2 center(bar->BarRect.Min.x + tab.Offset - bar->ScrollingAnim + tab.Width * 0.4f,
                            (bar->BarRect.Min.y + bar->BarRect.Max.y) * 0.5f);

        ImGuiIO& io = ImGui::GetIO();

        io.AddMousePosEvent(center.x, center.y);

        DrawFrame();

        io.AddMouseButtonEvent(button, true);

        DrawFrame();

        io.AddMouseButtonEvent(button, false);

        DrawFrame();

        DrawFrame();
    }

    EditorScene               scene;
    EditorSelection           selection{scene};
    InspectorNavigation       navigation{scene, selection};
    EditorActions             registry;
    InspectorNavigationWidget widget;
    InspectionTarget          node;
    InspectionTarget          material;
    InspectionTarget          texture;
    bool                      focused{true};
    bool                      inspectorFocused{true};
};

TEST_F(InspectorControls, AltArrowsNavigateWithoutChangingSelectionAndRespectNativeFocus)
{
    const uint64_t revision = selection.GetRevision();

    EXPECT_EQ(navigation.GetActiveTab().GetTarget(), material);

    AltArrow(ImGuiKey_LeftArrow);

    EXPECT_EQ(navigation.GetActiveTab().GetTarget(), node);

    AltArrow(ImGuiKey_RightArrow);

    EXPECT_EQ(navigation.GetActiveTab().GetTarget(), material);

    focused = false;

    AltArrow(ImGuiKey_LeftArrow);

    EXPECT_EQ(navigation.GetActiveTab().GetTarget(), material);

    EXPECT_EQ(selection.GetRevision(), revision);
}

TEST_F(InspectorControls, TabsCanBeSelectedAndClosedWithoutLosingTheSelectionPage)
{
    ASSERT_EQ(navigation.GetTabs().size(), 2u);

    const uint32_t referenceTab = navigation.GetActiveTab().id;

    ClickTab(0, ImGuiMouseButton_Left);

    EXPECT_EQ(navigation.GetActiveTab().id, 0u);

    EXPECT_EQ(navigation.GetActiveTab().GetTarget(), node);

    ClickTab(1, ImGuiMouseButton_Left);

    EXPECT_EQ(navigation.GetActiveTab().id, referenceTab);

    ClickTab(1, ImGuiMouseButton_Middle);

    EXPECT_EQ(navigation.GetTabs().size(), 1u);

    EXPECT_EQ(navigation.GetActiveTab().id, 0u);

    EXPECT_EQ(navigation.GetActiveTab().GetTarget(), node);
}

TEST_F(InspectorControls, WorkspaceShortcutsDoNotNavigateAnUnfocusedInspectorOrExecuteTwice)
{
    navigation.Open(texture);

    DrawFrame();

    inspectorFocused = false;

    DrawFrame();

    AltArrow(ImGuiKey_LeftArrow);

    EXPECT_EQ(navigation.GetActiveTab().GetTarget(), texture);

    inspectorFocused = true;

    DrawFrame();

    AltArrow(ImGuiKey_LeftArrow);

    EXPECT_EQ(navigation.GetActiveTab().GetTarget(), material);

    AltArrow(ImGuiKey_RightArrow);

    EXPECT_EQ(navigation.GetActiveTab().GetTarget(), texture);
}
} // namespace
} // namespace zen::editor
