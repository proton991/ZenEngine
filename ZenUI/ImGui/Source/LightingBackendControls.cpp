#include "ImGui/LightingBackendControls.h"
#include "imgui.h"

namespace zen::ui
{
bool DrawGIPresetControls(rc::VoxelGIRuntimeSettings& settings, const char* selectionReason)
{
    bool                    changed  = false;
    const rc::VoxelGIPreset selected = rc::GetVoxelGIPreset(settings);
    if (ImGui::BeginCombo("GI preset", rc::GetVoxelGIPresetName(selected)))
    {
        for (uint32_t index = 1; index <= static_cast<uint32_t>(rc::VoxelGIPreset::Ultra); ++index)
        {
            const rc::VoxelGIPreset preset = static_cast<rc::VoxelGIPreset>(index);
            if (ImGui::Selectable(rc::GetVoxelGIPresetName(preset), selected == preset))
            {
                changed = rc::ApplyVoxelGIPreset(preset, settings);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::TextWrapped(
        "Low: 64 cubed, specular occlusion only. Medium: 64 cubed with reflections. High: 128 cubed. Ultra: 256 cubed. All use full-resolution tracing.");
    ImGui::TextWrapped("Selection: %s. Backend controls can override the preset preference.", selectionReason);
    return changed;
}

bool DrawLightingBackendControls(rc::VoxelGISettings& settings, bool hardwareReady, const char* reason)
{
    bool changed = false;
    if (ImGui::CollapsingHeader("Lighting backends", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const uint32_t requested = settings.GetHardwareFeatureMask();
        const uint32_t all       = (1u << static_cast<uint32_t>(rc::VoxelGISettings::Feature::Count)) - 1u;
        const char*    overall   = requested == 0 ? "Compute" : requested == all ? "HW RT" : "Mixed";
        const char*    choices[] = {"Compute", "HW RT"};
        if (ImGui::BeginCombo("All features", overall))
        {
            for (int index = 0; index < 2; ++index)
            {
                if (ImGui::Selectable(choices[index], index == 0 ? requested == 0 : requested == all))
                {
                    settings.SetAllFeatureProviders(index == 0 ? rc::VoxelGISettings::RayProvider::Voxel
                                                               : rc::VoxelGISettings::RayProvider::Hardware);
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        const char* names[] = {"Environment lighting", "Specular occlusion", "Sky-cache visibility",
                               "Diffuse bounce",       "Glossy reflections", "Direct-light shadows"};
        for (uint32_t index = 0; index < static_cast<uint32_t>(rc::VoxelGISettings::Feature::Count); ++index)
        {
            const rc::VoxelGISettings::Feature feature = static_cast<rc::VoxelGISettings::Feature>(index);
            int selected = settings.GetFeatureProvider(feature) == rc::VoxelGISettings::RayProvider::Hardware ? 1 : 0;
            if (ImGui::Combo(names[index], &selected, choices, 2))
            {
                settings.SetFeatureProvider(feature, selected == 0 ? rc::VoxelGISettings::RayProvider::Voxel
                                                                   : rc::VoxelGISettings::RayProvider::Hardware);
                changed = true;
            }
        }
        ImGui::TextWrapped(
            "All features sets every selector. Individual changes show Mixed. Compute traces triangles in software; HW RT uses hardware queries. Both use the same lighting model.");
        if (settings.GetHardwareFeatureMask() != 0 && !hardwareReady)
        {
            ImGui::TextWrapped(
                "HW RT is not active; requested features use the Compute fallback until hardware queries are ready. Status: %s",
                reason);
        }
    }
    return changed;
}
} // namespace zen::ui
