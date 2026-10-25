#include "SceneLoadingDialog.h"
#include "Editor/ImGui/EditorTheme.h"
#include "imgui.h"
#include <algorithm>
#include <cstdio>

namespace zen::editor
{
SceneLoadingAction DrawSceneLoadingDialog(const SceneLoadState& state, const std::string& error)
{
    constexpr const char* title = "Opening Scene";

    SceneLoadingAction action   = SceneLoadingAction::None;

    if (state.stage != SceneLoadStage::Idle && !ImGui::IsPopupOpen(title))
    {
        ImGui::OpenPopup(title);
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    ImGui::SetNextWindowSize(ImVec2(std::min(500 * EditorScale(), viewport->Size.x - 40), 0));

    if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        if (state.stage == SceneLoadStage::Idle || state.stage == SceneLoadStage::Cancelled)
        {
            ImGui::CloseCurrentPopup();
        }
        else
        {
            if (state.recook)
            {
                ImGui::TextUnformatted("Recooking from source");
            }
            ImGui::TextWrapped("%s", state.path.c_str());

            ImGui::Spacing();

            if (state.stage == SceneLoadStage::Failed)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, GetEditorPalette().error);

                ImGui::TextWrapped("Could not %s scene: %s", state.recook ? "recook" : "open", error.c_str());

                ImGui::PopStyleColor();

                if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape))
                {
                    action = SceneLoadingAction::Dismiss;

                    ImGui::CloseCurrentPopup();
                }
            }
            else
            {
                const char* label = "Waiting to start...";

                switch (state.stage)
                {
                    case SceneLoadStage::Reading:
                        label = "Reading scene data...";
                        switch (state.importStage)
                        {
                            case asset::SceneImportStage::DecodingTextures: label = "Decoding textures..."; break;
                            case asset::SceneImportStage::ProcessingMeshes: label = "Processing meshes..."; break;
                            case asset::SceneImportStage::BakingAnimations: label = "Processing animations..."; break;
                            case asset::SceneImportStage::BuildingScene: label = "Building scene data..."; break;
                            case asset::SceneImportStage::CookingAssets: label = "Cooking assets..."; break;
                            case asset::SceneImportStage::SavingCache: label = "Saving cooked cache..."; break;
                            default: break;
                        }

                        break;
                    case SceneLoadStage::Preparing: label = "Preparing graphics resources..."; break;
                    case SceneLoadStage::Publishing: label = "Building scene views..."; break;
                    case SceneLoadStage::Complete: label = "Scene ready"; break;
                    default: break;
                }

                ImGui::TextUnformatted(state.cancelRequested ? "Cancelling..." : label);

                float progress    = 0;
                char  overlay[96] = "Discovering files...";
                if (state.fileProgress.totalKnown && state.fileProgress.totalFiles != 0)
                {
                    const uint64_t processed = std::min(state.fileProgress.processedFiles, state.fileProgress.totalFiles);
                    const double fraction = static_cast<double>(processed) / static_cast<double>(state.fileProgress.totalFiles);
                    const uint32_t percent =
                        processed == state.fileProgress.totalFiles
                            ? 100u
                            : std::min(99u, static_cast<uint32_t>(static_cast<double>(processed) * 100.0
                                                                  / static_cast<double>(state.fileProgress.totalFiles)));
                    progress = static_cast<float>(fraction);
                    std::snprintf(overlay, sizeof(overlay), "%u%% (%llu / %llu files)", percent,
                                  static_cast<unsigned long long>(processed),
                                  static_cast<unsigned long long>(state.fileProgress.totalFiles));
                }
                ImGui::ProgressBar(progress, ImVec2(-1, 0), overlay);
                if (state.stage != SceneLoadStage::Complete)
                {
                    ImGui::BeginDisabled(state.cancelRequested);
                    if (ImGui::Button("Cancel") || (!state.cancelRequested && ImGui::IsKeyPressed(ImGuiKey_Escape)))
                    {
                        action = SceneLoadingAction::Cancel;
                    }
                    ImGui::EndDisabled();
                }
            }
        }

        ImGui::EndPopup();
    }

    return action;
}
} // namespace zen::editor
