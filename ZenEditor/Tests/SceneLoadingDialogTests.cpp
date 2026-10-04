#include "SceneLoadingDialog.h"
#include "Editor/ImGui/EditorTheme.h"
#include "imgui.h"
#include <gtest/gtest.h>

namespace zen::editor
{
namespace
{
class SceneLoadingDialog : public testing::Test
{
protected:
    void SetUp() override
    {
        ImGui::CreateContext();

        ImGuiIO& io    = ImGui::GetIO();

        io.IniFilename = nullptr;

        io.DisplaySize = ImVec2(1000, 800);

        io.DeltaTime   = 1.0f / 60.0f;

        ApplyEditorTheme(1);

        unsigned char* pixels = nullptr;

        int width             = 0;

        int height            = 0;

        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

        io.Fonts->SetTexID(1);
    }

    void TearDown() override
    {
        ImGui::DestroyContext();
    }

    void DrawFrame(SceneLoadStage stage)
    {
        ImGui::NewFrame();

        ImGui::Begin("Loading test");

        dismissed = DrawSceneLoadingDialog({stage, "example.gltf"}, "Import failed");

        open      = ImGui::IsPopupOpen("Opening Scene");

        ImGui::End();

        ImGui::Render();
    }

    bool open{false};
    bool dismissed{false};
};

TEST_F(SceneLoadingDialog, ProgressRemainsModalThroughoutLoadingAndClosesWhenIdle)
{
    const SceneLoadStage stages[] = {SceneLoadStage::Queued, SceneLoadStage::Reading, SceneLoadStage::Preparing,
                                     SceneLoadStage::Publishing, SceneLoadStage::Complete};

    for (SceneLoadStage stage : stages)
    {
        DrawFrame(stage);

        EXPECT_TRUE(open);

        EXPECT_FALSE(dismissed);
    }

    DrawFrame(SceneLoadStage::Idle);

    EXPECT_FALSE(open);
}

TEST_F(SceneLoadingDialog, ImportFailureCanBeDismissed)
{
    DrawFrame(SceneLoadStage::Reading);

    DrawFrame(SceneLoadStage::Failed);

    EXPECT_TRUE(open);

    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);

    DrawFrame(SceneLoadStage::Failed);

    EXPECT_TRUE(dismissed);

    EXPECT_FALSE(open);

    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);

    DrawFrame(SceneLoadStage::Idle);

    EXPECT_FALSE(open);
}
} // namespace
} // namespace zen::editor
