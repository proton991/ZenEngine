#include "Editor/ImGui/EditorContext.h"
#include "Editor/ImGui/EditorTheme.h"
#include "Panels/EditorPanels.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RHI/RHIOptions.h"
#include <gtest/gtest.h>
#include <tuple>

namespace zen::editor
{
namespace
{
class EditorPreview : public testing::TestWithParam<std::tuple<RHIExecutionMode, bool>>
{};

void DrawPreviewPanel(EditorPanel& panel, EditorContext& context, bool collapsed)
{
    ImGui::NewFrame();

    if (panel.visible)
    {
        ImGui::SetNextWindowPos(ImVec2(0, 0));

        ImGui::SetNextWindowSize(ImVec2(900, 700));

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

    device.Init(nullptr);

    rc::ShaderProgramManager::GetInstance().BuildShaderPrograms(&device);

    device.InitializeRendererServer();

    EditorController editor(device);

    ASSERT_TRUE(editor.Init());

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

    renderer.Destroy();

    ImGui::DestroyContext();

    editor.Destroy();

    rc::ShaderProgramManager::GetInstance().Destroy();

    device.Destroy();
}

INSTANTIATE_TEST_SUITE_P(Panels,
                         EditorPreview,
                         testing::Combine(testing::Values(RHIExecutionMode::eInline, RHIExecutionMode::eThreaded),
                                          testing::Bool()));
} // namespace
} // namespace zen::editor
