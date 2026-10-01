#include "UI/RuntimeDebugUI.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <cctype>
#include <cstring>

namespace zen::ui
{
namespace
{
bool ContainsIgnoringCase(const std::string& value, const char* filter)
{
    const size_t length = std::strlen(filter);

    bool found = length == 0;

    for (size_t start = 0; !found && start + length <= value.size(); ++start)
    {
        bool matches = true;

        for (size_t offset = 0; matches && offset < length; ++offset)
        {
            const unsigned char left = static_cast<unsigned char>(value[start + offset]);

            const unsigned char right = static_cast<unsigned char>(filter[offset]);

            matches = std::tolower(left) == std::tolower(right);
        }

        found = matches;
    }

    return found;
}

const char* ModelLabel(const RuntimeModelState& state, const std::string& path)
{
    const char* label = path.c_str();

    for (const asset::GLTFModelCatalogEntry& entry : state.models)
    {
        if (entry.path == path)
        {
            label = entry.label.c_str();
        }
    }

    return label;
}

std::string ModelName(const std::string& path)
{
    const size_t separator = path.find_last_of("/\\");

    const size_t start = separator == std::string::npos ? 0 : separator + 1;

    const size_t extension = path.find_last_of('.');

    const size_t end =
        extension != std::string::npos && extension > start ? extension : path.size();

    return path.substr(start, end - start);
}
} // namespace

void RuntimeDebugUI::SynchronizeModelRevision()
{
    if (m_modelRevision != m_sceneControls.GetRuntimeModelState().revision)
    {
        ReloadSettings();

        m_modelRequestRejected = false;

        m_status = "Model loaded. Scene controls refreshed.";
    }
}

bool RuntimeDebugUI::MatchesModelSearch(const asset::GLTFModelCatalogEntry& entry) const
{
    return ContainsIgnoringCase(entry.label, m_modelFilter) ||
        ContainsIgnoringCase(entry.path, m_modelFilter);
}

bool RuntimeDebugUI::RequestModel(const std::string& path)
{
    const RuntimeModelState& state = m_sceneControls.GetRuntimeModelState();

    bool requested = false;

    if (state.pendingPath.empty() && !path.empty() && path != state.currentPath)
    {
        requested = m_sceneControls.RequestRuntimeModel(path);

        m_modelRequestRejected = !requested;
    }

    return requested;
}

void RuntimeDebugUI::BuildModelSelector()
{
    const RuntimeModelState& state = m_sceneControls.GetRuntimeModelState();

    const bool loading = !state.pendingPath.empty();

    ImGui::BeginDisabled(loading);

    if (ImGui::Button("Refresh models"))
    {
        m_sceneControls.RefreshRuntimeModels();

        m_modelRequestRejected = false;
    }

    ImGui::EndDisabled();

    if (state.basePath.empty())
    {
        ImGui::TextDisabled("Model directory is not configured.");
    }
    else
    {
        ImGui::TextWrapped("Directory: %s", state.basePath.c_str());
    }

    ImGui::BeginDisabled(loading);

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);

    ImGui::InputTextWithHint("##Model search", "Search model or variant path", m_modelFilter,
                             sizeof(m_modelFilter));

    size_t matching = 0;

    for (const asset::GLTFModelCatalogEntry& entry : state.models)
    {
        matching += MatchesModelSearch(entry) ? 1 : 0;
    }

    ImGui::BeginDisabled(matching == 0);

    const ImGuiStyle& style = ImGui::GetStyle();

    const ImVec2 padding = style.FramePadding;

    const float width = ImGui::GetContentRegionAvail().x;

    const float textWidth = ImMax(1.0f, width - padding.x * 3.0f - ImGui::GetFontSize());

    const std::string name =
        state.currentPath.empty() ? "No model loaded" : ModelName(state.currentPath);

    const ImVec2 nameSize = ImGui::CalcTextSize(name.c_str(), nullptr, false, textWidth);

    const float pathHeight = state.currentPath.empty() ?
        0.0f :
        ImGui::CalcTextSize(state.currentPath.c_str(), nullptr, false, textWidth).y;

    const float pathSpacing = state.currentPath.empty() ? 0.0f : style.ItemSpacing.y;

    const float previewHeight = nameSize.y + pathSpacing + pathHeight + padding.y * 2.0f;

    ImGui::SetNextItemWidth(width);

    // Size the frame for both lines while keeping normal text padding and a small arrow.
    // The custom preview renders paths literally, including ImGui's ##/### syntax.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(padding.x, (previewHeight - ImGui::GetFontSize()) * 0.5f));

    const bool open =
        ImGui::BeginCombo("##Model selection", nullptr,
                          ImGuiComboFlags_HeightLarge | ImGuiComboFlags_NoArrowButton |
                              ImGuiComboFlags_CustomPreview);

    ImGui::PopStyleVar();

    if (open)
    {
        for (size_t index = 0; index < state.models.size(); ++index)
        {
            const asset::GLTFModelCatalogEntry& entry = state.models[index];

            if (MatchesModelSearch(entry))
            {
                const bool selected = entry.path == state.currentPath;

                // Path text may contain ImGui's ##/### label or ID syntax.
                // Keep identity separate and draw the complete literal path.
                ImGui::PushID(static_cast<int>(index));

                const ImVec2 textPosition = ImGui::GetCursorPos();

                const ImVec2 textSize = ImGui::CalcTextSize(entry.label.c_str(), nullptr, false,
                                                            ImGui::GetContentRegionAvail().x);

                if (ImGui::Selectable("##Model entry", selected, 0, ImVec2(0, textSize.y)))
                {
                    RequestModel(entry.path);
                }

                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("%s", entry.path.c_str());
                }

                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }

                ImGui::SetCursorPos(textPosition);

                ImGui::PushTextWrapPos(0);

                ImGui::TextUnformatted(entry.label.c_str());

                ImGui::PopTextWrapPos();

                ImGui::PopID();
            }
        }

        ImGui::EndCombo();
    }

    if (ImGui::BeginComboPreview())
    {
        const ImVec2 position = ImGui::GetCursorScreenPos();

        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + textWidth);

        ImGui::TextUnformatted(name.c_str());

        if (!state.currentPath.empty())
        {
            ImGui::SetCursorScreenPos(ImVec2(position.x, position.y + nameSize.y + pathSpacing));

            ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_TextDisabled]);

            ImGui::TextUnformatted(state.currentPath.c_str());

            ImGui::PopStyleColor();
        }

        ImGui::PopTextWrapPos();

        const ImRect& preview = ImGui::GetCurrentContext()->ComboPreviewData.PreviewRect;

        ImGui::RenderArrow(ImGui::GetWindowDrawList(),
                           ImVec2(preview.Max.x - padding.x - ImGui::GetFontSize(), position.y),
                           ImGui::GetColorU32(ImGuiCol_Text), ImGuiDir_Down);

        ImGui::EndComboPreview();
    }

    ImGui::EndDisabled();

    ImGui::EndDisabled();

    if (loading)
    {
        ImGui::TextWrapped("Loading: %s", ModelLabel(state, state.pendingPath));
    }
    else if (state.models.empty())
    {
        ImGui::TextWrapped(
            "No .gltf or .glb models found. Check the directory and Refresh models.");
    }
    else
    {
        ImGui::TextDisabled("%zu of %zu models", matching, state.models.size());

        if (matching == 0)
        {
            ImGui::TextUnformatted("No models match this search.");
        }
    }

    if (!state.error.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.4f, 1.0f), "Model load failed:");

        ImGui::TextWrapped("%s", state.error.c_str());
    }
    else if (m_modelRequestRejected)
    {
        ImGui::TextWrapped("Could not queue this model. Refresh models and try again.");
    }
}
} // namespace zen::ui
