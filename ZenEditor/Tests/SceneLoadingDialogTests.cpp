#include "SceneLoadingDialog.h"
#include "Editor/ImGui/EditorTheme.h"
#include "imgui.h"
#include "imgui_internal.h"
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

    void DrawFrame(SceneLoadStage                 stage,
                   asset::SceneImportStage        importStage  = asset::SceneImportStage::Queued,
                   bool                           recook       = false,
                   asset::SceneImportFileProgress fileProgress = {})
    {
        ImGui::NewFrame();

        ImGui::Begin("Loading test");

        SceneLoadState state{stage, "example.gltf"};
        state.importStage  = importStage;
        state.recook       = recook;
        state.fileProgress = fileProgress;
        ImGui::LogToBuffer();
        action       = DrawSceneLoadingDialog(state, "Import failed");
        renderedText = ImGui::GetCurrentContext()->LogBuffer.c_str();
        ImGui::LogFinish();
        dismissed = action == SceneLoadingAction::Dismiss;

        open      = ImGui::IsPopupOpen("Opening Scene");

        ImGui::End();

        ImGui::Render();
    }

    bool               open{false};
    bool               dismissed{false};
    SceneLoadingAction action{SceneLoadingAction::None};
    std::string        renderedText;
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

TEST_F(SceneLoadingDialog, RecookCookingAndCachePublicationRemainCancellable)
{
    for (asset::SceneImportStage stage : {asset::SceneImportStage::CookingAssets, asset::SceneImportStage::SavingCache})
    {
        DrawFrame(SceneLoadStage::Reading, stage, true);
        EXPECT_TRUE(open);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
        DrawFrame(SceneLoadStage::Reading, stage, true);
        EXPECT_EQ(action, SceneLoadingAction::Cancel);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
        DrawFrame(SceneLoadStage::Cancelled, stage, true);
        EXPECT_FALSE(open);
    }
}

TEST_F(SceneLoadingDialog, PercentageTracksFileCountsWithinTheSameImportStage)
{
    DrawFrame(SceneLoadStage::Reading);
    DrawFrame(SceneLoadStage::Reading, asset::SceneImportStage::DecodingTextures, false, {37, 74, true});
    EXPECT_NE(renderedText.find("50% (37 / 74 files)"), std::string::npos) << renderedText;
    EXPECT_EQ(renderedText.find("Step "), std::string::npos);

    DrawFrame(SceneLoadStage::Reading, asset::SceneImportStage::DecodingTextures, false, {29, 100, true});
    EXPECT_NE(renderedText.find("29% (29 / 100 files)"), std::string::npos) << renderedText;

    DrawFrame(SceneLoadStage::Reading, asset::SceneImportStage::DecodingTextures, false, {299, 300, true});
    EXPECT_NE(renderedText.find("99% (299 / 300 files)"), std::string::npos) << renderedText;
    DrawFrame(SceneLoadStage::Reading, asset::SceneImportStage::DecodingTextures, false, {300, 300, true});
    EXPECT_NE(renderedText.find("100% (300 / 300 files)"), std::string::npos) << renderedText;
}

TEST_F(SceneLoadingDialog, DiscoveryDoesNotPresentAPercentageFromAnIncompleteTotal)
{
    DrawFrame(SceneLoadStage::Reading);
    DrawFrame(SceneLoadStage::Reading, asset::SceneImportStage::Reading, false, {5, 10, false});
    EXPECT_NE(renderedText.find("Discovering files..."), std::string::npos) << renderedText;
    EXPECT_EQ(renderedText.find('%'), std::string::npos);
}
} // namespace
} // namespace zen::editor
