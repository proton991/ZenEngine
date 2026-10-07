#include "Editor/ImGui/EditorContext.h"
#include "Editor/ImGui/EditorTheme.h"
#include "Editor/ImGui/EditorWorkspace.h"
#include "Panels/EditorPanels.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RHI/RHIOptions.h"
#include "imgui_internal.h"
#include <gtest/gtest.h>
#include <tuple>

namespace zen::editor
{
namespace
{
class EditorPreview : public testing::TestWithParam<std::tuple<RHIExecutionMode, bool>>
{};

void InitializePreviewDevice(rc::RenderDevice& device)
{
    device.Init(nullptr);

    rc::ShaderProgramManager::GetInstance().BuildShaderPrograms(&device);

    device.InitializeRendererServer();
}

void InitializePreviewUI()
{
    ImGui::CreateContext();

    ImGuiIO& io    = ImGui::GetIO();

    io.IniFilename = nullptr;

    io.DisplaySize = ImVec2(900, 700);

    io.DeltaTime   = 1.0f / 60.0f;

    ApplyEditorTheme(1);

    unsigned char* pixels = nullptr;

    int width             = 0;

    int height            = 0;

    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    io.Fonts->SetTexID(1);
}

void DestroyPreviewTest(EditorController& editor, ui::ImGuiRenderer& renderer, rc::RenderDevice& device)
{
    renderer.Destroy();

    ImGui::DestroyContext();

    editor.Destroy();

    rc::ShaderProgramManager::GetInstance().Destroy();

    device.Destroy();
}

void DrawPreviewPanel(EditorPanel& panel, EditorContext& context, bool collapsed, ImVec2 extent = ImVec2(900, 700))
{
    ImGui::NewFrame();

    if (panel.visible)
    {
        ImGui::SetNextWindowPos(ImVec2(0, 0));

        ImGui::SetNextWindowSize(extent);

        ImGui::SetNextWindowCollapsed(collapsed);
    }

    panel.Draw(context);

    ImGui::Render();
}

TEST_P(EditorPreview, SceneSwitchRetiresPreviewsWithoutRequestingAnotherThumbnail)
{
    platform::NativeWindow window({"Editor preview lifecycle test", false, 900, 700, 0, false});

    EditorWindowChrome chrome(window);

    RHIOptions::GetInstance().SetRayTracingEnabled(false);

    RHIOptions::GetInstance().SetValidationEnabled(true);

    rc::RenderDevice device(RHIAPIType::eVulkan, 2, std::get<0>(GetParam()));

    InitializePreviewDevice(device);

    EditorController editor(device);

    ASSERT_TRUE(editor.Init());

    InitializePreviewUI();

    ui::ImGuiRenderer renderer(device);

    EditorLog log;

    EditorContext context{editor, log, renderer, chrome};

    // Visible but empty, hidden, and collapsed panels must all release old images.
    for (uint32_t state = 0; state < 3; ++state)
    {
        ASSERT_TRUE(editor.Load(std::string(ZEN_EDITOR_FIXTURES) + "textured.gltf"));

        SceneAssetId texture;

        for (const SceneAssetItem& item : editor.GetScene().GetAssets().Query(""))
        {
            if (item.id.kind == SceneAssetKind::Texture)
            {
                texture = item.id;
            }
        }

        editor.GetSelection().SelectAsset(texture);

        RHIResourcePtr<RHITexture> preview(editor.GetViewport().GetAssetPreview(texture));

        ASSERT_NE(preview.Get(), nullptr);

        ASSERT_TRUE(device.PrepareForResourceReconfiguration());

        const uint32_t before        = preview->GetRefCount();

        UniquePtr<EditorPanel> panel = std::get<1>(GetParam()) ? CreateInspectorPanel() : CreateAssetsPanel();

        DrawPreviewPanel(*panel, context, false);

        DrawPreviewPanel(*panel, context, false);

        ASSERT_GT(preview->GetRefCount(), before);

        ASSERT_TRUE(editor.Load(std::string(ZEN_EDITOR_FIXTURES) + "empty.gltf"));

        panel->visible = state != 1;

        DrawPreviewPanel(*panel, context, state == 2);

        ASSERT_TRUE(device.PrepareForResourceReconfiguration());

        // Only this test's observing reference remains, even while the panel lives.
        EXPECT_EQ(preview->GetRefCount(), 1u);
    }

    DestroyPreviewTest(editor, renderer, device);
}

TEST_P(EditorPreview, SceneViewportStaysStableAcrossProfilingAndLightPresetChanges)
{
    platform::NativeWindow window({"Scene layout test", false, 900, 700, 0, false});
    EditorWindowChrome     chrome(window);
    RHIOptions::GetInstance().SetRayTracingEnabled(false);
    RHIOptions::GetInstance().SetValidationEnabled(true);
    rc::RenderDevice device(RHIAPIType::eVulkan, 2, std::get<0>(GetParam()));
    InitializePreviewDevice(device);
    EditorController editor(device);
    ASSERT_TRUE(editor.Init());
    ASSERT_TRUE(editor.Load(std::string(ZEN_EDITOR_FIXTURES) + "inspection.gltf"));
    InitializePreviewUI();
    ui::ImGuiRenderer renderer(device);
    EditorLog         log;
    EditorContext     context{editor, log, renderer, chrome};
    context.focused              = false;
    UniquePtr<EditorPanel> panel = CreateScenePanel();

    for (const float width : {240.0f, 500.0f, 604.0f, 606.0f, 612.0f, 720.0f, 900.0f})
    {
        SCOPED_TRACE(width);
        rc::RenderingSettings draft = editor.GetRenderingState().GetDraft();
        draft.algorithm             = rc::RenderAlgorithm::ePBR;
        draft.lights.clear();
        ASSERT_TRUE(editor.StageRenderingSettings(draft));
        ASSERT_TRUE(editor.UpdateRenderingSettings());
        const ImVec2 extent(width, 500);
        DrawPreviewPanel(*panel, context, false, extent);
        DrawPreviewPanel(*panel, context, false, extent);
        const uint64_t targetRevision = editor.GetViewport().GetTargetRevision();
        const uint64_t cameraRevision = editor.GetCamera().GetRevision();
        for (int edit = 0; edit < 4; ++edit)
        {
            if (edit == 1 || edit == 2)
            {
                ASSERT_TRUE(editor.AddBoundsLights(std::get<1>(GetParam())));
                ASSERT_TRUE(editor.UpdateRenderingSettings());
            }
            else if (edit == 3)
            {
                draft = editor.GetRenderingState().GetDraft();
                draft.lights.clear();
                ASSERT_TRUE(editor.StageRenderingSettings(draft));
                ASSERT_TRUE(editor.UpdateRenderingSettings());
            }
            for (const float ms : {0.1f, 1.0f, 10.0f, 16.0f, 100.0f, 1000.0f})
            {
                context.frameMs = ms;
                DrawPreviewPanel(*panel, context, false, extent);
                EXPECT_EQ(editor.GetViewport().GetTargetRevision(), targetRevision) << ms;
                EXPECT_EQ(editor.GetCamera().GetRevision(), cameraRevision) << ms;
            }
        }
    }
    panel.Reset();
    DestroyPreviewTest(editor, renderer, device);
}

void DrawDockedPanels(const HeapVector<UniquePtr<EditorPanel>>& panels, EditorContext& context)
{
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(0, 0));

    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);

    ImGui::Begin("Panel selection test", nullptr, ImGuiWindowFlags_NoDecoration);

    const ImGuiID dockspace = ImGui::GetID("PanelSelectionDockspace");

    if (!HasEditorLayout(dockspace))
    {
        BuildDefaultEditorLayout(dockspace, 900, 700, panels);
    }

    ImGui::DockSpace(dockspace);

    ImGui::End();

    for (const UniquePtr<EditorPanel>& panel : panels)
    {
        panel->Draw(context);
    }

    ImGui::Render();
}

TEST_P(EditorPreview, SelectionRevealsDockedInspectorWithoutOverridingManualTabChanges)
{
    platform::NativeWindow window({"Editor panel selection test", false, 900, 700, 0, false});

    EditorWindowChrome chrome(window);

    RHIOptions::GetInstance().SetRayTracingEnabled(false);

    RHIOptions::GetInstance().SetValidationEnabled(true);

    rc::RenderDevice device(RHIAPIType::eVulkan, 2, std::get<0>(GetParam()));

    InitializePreviewDevice(device);

    EditorController editor(device);

    ASSERT_TRUE(editor.Init());

    ASSERT_TRUE(editor.Load(std::string(ZEN_EDITOR_FIXTURES) + "textured.gltf"));

    InitializePreviewUI();

    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ui::ImGuiRenderer renderer(device);

    EditorLog log;

    EditorContext context{editor, log, renderer, chrome};

    SceneAssetId texture;

    for (const SceneAssetItem& item : editor.GetScene().GetAssets().Query(""))
    {
        if (item.id.kind == SceneAssetKind::Texture)
        {
            texture = item.id;
        }
    }

    ASSERT_NE(texture.generation, 0u);

    const NodeId node = editor.GetScene().GetRoots()[0];

    HeapVector<UniquePtr<EditorPanel>> panels;

    panels.push_back(CreateRenderSettingsPanel());

    panels.push_back(CreateInspectorPanel());

    panels.push_back(std::get<1>(GetParam()) ? CreateAssetsPanel() : CreateHierarchyPanel());

    DrawDockedPanels(panels, context);

    DrawDockedPanels(panels, context);

    ImGuiWindow* rendering = ImGui::FindWindowByName(panels[0]->GetWindowName().c_str());

    ImGuiWindow* inspector = ImGui::FindWindowByName(panels[1]->GetWindowName().c_str());

    ASSERT_NE(rendering, nullptr);

    ASSERT_NE(inspector, nullptr);

    ASSERT_NE(inspector->DockNode, nullptr);

    ASSERT_EQ(rendering->DockNode, inspector->DockNode);

    for (uint32_t attempt = 0; attempt < 3; ++attempt)
    {
        ImGui::SetWindowFocus(rendering->Name);

        DrawDockedPanels(panels, context);

        DrawDockedPanels(panels, context);

        EXPECT_EQ(inspector->DockNode->VisibleWindow, rendering);

        if (attempt == 2)
        {
            panels[1]->visible = false;

            DrawDockedPanels(panels, context);
        }

        // These are the same selection entry points used by Assets, Hierarchy and scene picks.
        if (std::get<1>(GetParam()))
        {
            editor.GetSelection().SelectAsset(texture);
        }
        else
        {
            editor.GetSelection().SelectNode(node);
        }

        DrawDockedPanels(panels, context);

        DrawDockedPanels(panels, context);

        EXPECT_TRUE(panels[1]->visible);

        EXPECT_EQ(inspector->DockNode->VisibleWindow, inspector);

        EXPECT_EQ(editor.GetInspector().GetActiveTab().id, 0u);

        EXPECT_EQ(editor.GetInspector().GetActiveTab().GetTarget(),
                  (std::get<1>(GetParam()) ? InspectionTarget{{}, texture} : InspectionTarget{node, {}}));

        if (attempt == 0)
        {
            const InspectionTarget reference =
                std::get<1>(GetParam()) ? InspectionTarget{node, {}} : InspectionTarget{{}, texture};

            editor.GetInspector().Open(reference);

            DrawDockedPanels(panels, context);

            EXPECT_EQ(editor.GetInspector().GetTabs().size(), 2u);
        }
        else
        {
            EXPECT_EQ(editor.GetInspector().GetTabs().size(), 2u);
        }
    }

    panels.clear();

    DestroyPreviewTest(editor, renderer, device);
}

// The Output panel's scrolling child window.
ImGuiWindow* FindOutputLines(const EditorPanel& panel)
{
    const ImGuiWindow* output = ImGui::FindWindowByName(panel.GetWindowName().c_str());

    ImGuiWindow* result       = nullptr;

    for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows)
    {
        if (window->ParentWindow == output && std::string(window->Name).find("/LogLines_") != std::string::npos)
        {
            result = window;
        }
    }

    return result;
}

TEST_P(EditorPreview, OutputRowsCoverMultiLineEntriesAndFollowNewEntries)
{
    platform::NativeWindow window({"Editor output test", false, 900, 700, 0, false});

    EditorWindowChrome chrome(window);

    RHIOptions::GetInstance().SetRayTracingEnabled(false);

    RHIOptions::GetInstance().SetValidationEnabled(true);

    rc::RenderDevice device(RHIAPIType::eVulkan, 2, std::get<0>(GetParam()));

    InitializePreviewDevice(device);

    EditorController editor(device);

    ASSERT_TRUE(editor.Init());

    InitializePreviewUI();

    ui::ImGuiRenderer renderer(device);

    EditorLog log;

    EditorContext context{editor, log, renderer, chrome};

    UniquePtr<EditorPanel> panel = CreateOutputPanel();

    // Every third entry spans three lines, as the render graph's metrics do.
    for (int index = 0; index < 60; ++index)
    {
        log.Append(index % 3 == 0 ? 4 : 2, index % 3 == 0 ? "metrics\n  group=0\n  details" : "entry");
    }

    const uint32_t rows = 20 * 3 + 40;

    for (int frame = 0; frame < 3; ++frame)
    {
        DrawPreviewPanel(*panel, context, false, ImVec2(600, 300));
    }

    ImGuiWindow* lines = FindOutputLines(*panel);

    ASSERT_NE(lines, nullptr);

    // Each line is one clipped row; a row per entry would leave the last lines unreachable.
    const float spacing = ImGui::GetStyle().ItemSpacing.y;

    EXPECT_NEAR(lines->ContentSize.y, float(rows) * (ImGui::GetTextLineHeight() + spacing) - spacing, 1.0f);

    EXPECT_GT(lines->ScrollMax.y, 0.0f);

    EXPECT_FLOAT_EQ(lines->Scroll.y, lines->ScrollMax.y);

    // New entries keep a view at the bottom there.
    log.Append(4, "late\n  detail");

    for (int frame = 0; frame < 2; ++frame)
    {
        DrawPreviewPanel(*panel, context, false, ImVec2(600, 300));
    }

    EXPECT_NEAR(lines->ContentSize.y, float(rows + 2) * (ImGui::GetTextLineHeight() + spacing) - spacing, 1.0f);

    EXPECT_FLOAT_EQ(lines->Scroll.y, lines->ScrollMax.y);

    // A view scrolled up stays where the reader left it.
    ImGui::SetScrollY(lines, 0.0f);

    DrawPreviewPanel(*panel, context, false, ImVec2(600, 300));

    log.Append(2, "later");

    for (int frame = 0; frame < 2; ++frame)
    {
        DrawPreviewPanel(*panel, context, false, ImVec2(600, 300));
    }

    EXPECT_FLOAT_EQ(lines->Scroll.y, 0.0f);

    panel.Reset();

    DestroyPreviewTest(editor, renderer, device);
}

INSTANTIATE_TEST_SUITE_P(Panels,
                         EditorPreview,
                         testing::Combine(testing::Values(RHIExecutionMode::eInline, RHIExecutionMode::eThreaded),
                                          testing::Bool()));
} // namespace
} // namespace zen::editor
