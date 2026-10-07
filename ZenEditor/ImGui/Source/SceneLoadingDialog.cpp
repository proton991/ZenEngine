#include "SceneLoadingDialog.h"
#include "Editor/ImGui/EditorTheme.h"
#include "imgui.h"
#include <algorithm>

namespace zen::editor
{
bool DrawSceneLoadingDialog(const SceneLoadState& state, const std::string& error)
{
    constexpr const char* title = "Opening Scene";

    bool dismissed              = false;

    if (state.stage != SceneLoadStage::Idle && !ImGui::IsPopupOpen(title))
    {
        ImGui::OpenPopup(title);
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    ImGui::SetNextWindowSize(ImVec2(std::min(500 * EditorScale(), viewport->Size.x - 40), 0));

    if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        if (state.stage == SceneLoadStage::Idle)
        {
            ImGui::CloseCurrentPopup();
        }
        else
        {
            ImGui::TextWrapped("%s", state.path.c_str());

            ImGui::Spacing();

            if (state.stage == SceneLoadStage::Failed)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, GetEditorPalette().error);

                ImGui::TextWrapped("Could not open scene: %s", error.c_str());

                ImGui::PopStyleColor();

                if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape))
                {
                    dismissed = true;

                    ImGui::CloseCurrentPopup();
                }
            }
            else
            {
                const char* label = "Waiting to start...";

                const char* step  = "";

                float progress    = 0;

                switch (state.stage)
                {
                    case SceneLoadStage::Reading:
                        label = "Reading scene data...";

                        step  = "Step 1 of 3";

                        // No byte-based estimate is available while the importer is running.
                        progress = -float(ImGui::GetTime()) - 1.0f;

                        break;
                    case SceneLoadStage::Preparing:
                        label    = "Preparing graphics resources...";

                        step     = "Step 2 of 3";

                        progress = 1.0f / 3;

                        break;
                    case SceneLoadStage::Publishing:
                        label    = "Building scene views...";

                        step     = "Step 3 of 3";

                        progress = 2.0f / 3;

                        break;
                    case SceneLoadStage::Complete:
                        label    = "Scene ready";

                        step     = "Complete";

                        progress = 1;

                        break;
                    default: break;
                }

                ImGui::TextUnformatted(label);

                ImGui::ProgressBar(progress, ImVec2(-1, 0), step);
            }
        }

        ImGui::EndPopup();
    }

    return dismissed;
}
} // namespace zen::editor
