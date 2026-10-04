#include "Panels/EditorPanels.h"
#include "EditorWidgets.h"

namespace zen::editor
{
namespace
{
class CameraSettingsPanel final : public EditorPanel
{
public:
    CameraSettingsPanel() : EditorPanel({"CameraSettings", "Camera Settings", EditorDockArea::Right, false}) {}

private:
    void DrawSpeed(EditorPreferences& preferences)
    {
        ImGui::TextUnformatted("Move speed");

        ImGui::TextDisabled("Scene units per second");

        ImGui::Spacing();

        float speed = preferences.cameraMoveSpeed;

        ImGui::SetNextItemWidth(EditorControlWidth(196));

        if (ImGui::InputFloat("##MoveSpeed", &speed, 0.1f, 1.0f, "%.3f"))
        {
            preferences.cameraMoveSpeed = ClampEditorCameraMoveSpeed(speed);
        }

        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Type a speed (0.001 to 100), or use -/+ to change it by 0.1 (Ctrl: 1.0).\n"
                              "Applies immediately to RMB + WASD/QE.");
        }

        SameLineIfFits(ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2);

        ImGui::BeginDisabled(preferences.cameraMoveSpeed == kDefaultEditorCameraMoveSpeed);

        if (ImGui::Button("Reset"))
        {
            preferences.cameraMoveSpeed = kDefaultEditorCameraMoveSpeed;
        }

        ImGui::EndDisabled();

        ImGui::Spacing();

        ImGui::TextDisabled("Shift boost: %.3f units/s", preferences.cameraMoveSpeed * 3.0f);
    }

    void DrawShortcuts()
    {
        ImGui::TextUnformatted("Navigation shortcuts");

        ImGui::Spacing();

        if (ImGui::BeginTable("Shortcuts", 2, ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthStretch, 1.4f);

            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 1.0f);

            const char* shortcuts[][2] = {
                {"RMB + WASD / QE", "Fly"}, {"Shift", "Speed boost"}, {"Alt + LMB", "Orbit"}, {"MMB / wheel", "Pan / zoom"}};

            for (const char* const* shortcut : shortcuts)
            {
                ImGui::TableNextRow();

                ImGui::TableNextColumn();

                ImGui::TextWrapped("%s", shortcut[0]);

                ImGui::TableNextColumn();

                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);

                ImGui::TextWrapped("%s", shortcut[1]);

                ImGui::PopStyleColor();
            }

            ImGui::EndTable();
        }
    }

    void DrawContents(EditorContext& context) override
    {
        ImGui::TextColored(GetEditorPalette().brand, "CAMERA NAVIGATION");

        ImGui::TextDisabled("Shared across all scenes");

        ImGui::Spacing();

        const float width = EditorControlWidth(360);

        if (ImGui::BeginChild("Movement", ImVec2(width, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY))
        {
            DrawSpeed(context.editor.GetPreferences());
        }

        ImGui::EndChild();

        ImGui::Spacing();

        if (ImGui::BeginChild("Navigation", ImVec2(width, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY))
        {
            DrawShortcuts();
        }

        ImGui::EndChild();

        ImGui::Spacing();

        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);

        ImGui::TextDisabled("One scene unit is the model's longest extent. Framing a selection keeps your chosen speed.");

        ImGui::Spacing();

        ImGui::TextDisabled("Changes apply immediately. Saved between sessions.");

        ImGui::PopTextWrapPos();
    }
};
} // namespace

UniquePtr<EditorPanel> CreateCameraSettingsPanel()
{
    return MakeUnique<CameraSettingsPanel>();
}
} // namespace zen::editor
