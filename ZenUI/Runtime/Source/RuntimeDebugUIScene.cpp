#include "RuntimeUI/RuntimeDebugUI.h"
#include "Platform/ConfigLoader.h"
#include "imgui.h"
#include <algorithm>
#include <cstring>

namespace zen::ui
{
namespace
{
struct ConfigReference
{
    const char* key;
    const char* fallback;
    const char* controls;
};

const ConfigReference References[]      = {{"model_base_path", "<unset>", "Model selector / Catalog directory"},
                                           {"default_model", "<unset>", "Model selector / Startup model"},
                                           {"default_model_path", "<uses default_model>", "Model selector / Startup override"},
                                           {"skybox_model", "<unset>", "Legacy asset path; restart required"},
                                           {"camera_position", "<scene bounds>", "Scene / Camera"},
                                           {"voxelizer", "auto", "GI / Voxel resources"},
                                           {"async_compute", "off", "GI / Voxel resources"},
                                           {"voxel_resolution", "256", "GI / Voxel resources"},
                                           {"voxel_reflectance_policy", "owner", "GI / Voxel resources"},
                                           {"voxel_reflectance_budget_mb", "0", "GI / Voxel resources"},
                                           {"environment_texture", "papermill.ktx", "Restart required"},
                                           {"environment_lighting", "true", "Scene / Environment"},
                                           {"environment_intensity", "1", "Scene / Environment"},
                                           {"environment_rotation_degrees", "0", "Scene / Environment"},
                                           {"skybox_visible", "true", "Scene / Environment"},
                                           {"voxel_gi_analytic_lighting", "true", "GI / GI contributions"},
                                           {"voxel_gi_environment_lighting", "true", "GI / GI contributions"},
                                           {"voxel_gi_emissive_lighting", "true", "GI / GI contributions"},
                                           {"voxel_gi_indirect_intensity", "1", "GI / Indirect intensity"},
                                           {"voxel_gi_cone_count", "6", "GI / Cone tracing"},
                                           {"voxel_gi_cone_angle_degrees", "60", "GI / Cone tracing"},
                                           {"voxel_gi_step_scale", "1", "GI / Cone tracing"},
                                           {"voxel_gi_normal_bias_voxels", "1.5", "GI / Cone tracing"},
                                           {"voxel_gi_max_distance_grid_lengths", "1.7321", "GI / Cone tracing"},
                                           {"voxel_gi_max_steps", "128", "GI / Cone tracing"},
                                           {"voxel_gi_shadow_enabled", "true", "GI / Shadows"},
                                           {"shadow_map_resolution", "1024", "GI / Voxel resources"},
                                           {"light_count", "4 (legacy positions)", "Scene / Lights"},
                                           {"light_markers.enabled", "false", "Scene / Lights"},
                                           {"light_markers.size", "0.02", "Scene / Lights"},
                                           {"dynamic_light.enabled", "false", "Scene / Light animation"},
                                           {"dynamic_light.index", "0", "Scene / Light animation"},
                                           {"dynamic_light.orbit_center", "0,1,0", "Scene / Light animation"},
                                           {"dynamic_light.orbit_radius", "1", "Scene / Light animation"},
                                           {"dynamic_light.angular_speed_degrees", "45", "Scene / Light animation"}};

const ConfigReference LightReferences[] = {{"type", "point", "point, directional, spot"},
                                           {"position", "0,0,0", "Renderer world coordinates"},
                                           {"direction", "0,-1,0", "Nonzero direction"},
                                           {"color", "1,1,1", "Linear RGB"},
                                           {"intensity", "5", "Nonnegative"},
                                           {"range", "1000", "Positive"},
                                           {"inner_angle_degrees", "20", "Spot: 0 <= inner < outer"},
                                           {"outer_angle_degrees", "35", "Spot: outer < 90"},
                                           {"enabled", "true", "Enabled"},
                                           {"casts_shadows", "true", "Casts shadows"}};

void ReferenceRow(const char* key, const char* fallback, const char* controls, const char* filter)
{
    if (filter[0] == '\0' || std::strstr(key, filter) != nullptr)
    {
        const platform::ConfigLoader& config = platform::ConfigLoader::GetInstance();

        const std::string value              = config.GetString(key, fallback);

        ImGui::TableNextRow();

        ImGui::TableNextColumn();

        ImGui::TextWrapped("%s", key);

        ImGui::TableNextColumn();

        ImGui::TextWrapped("%s%s", value.c_str(), config.HasKey(key) ? "" : " (default)");

        ImGui::TableNextColumn();

        ImGui::TextWrapped("%s", controls);
    }
}

bool StepCount(const char* label, uint32_t& value, uint32_t maximum)
{
    bool changed = false;

    ImGui::PushID(label);

    ImGui::TextUnformatted(label);

    ImGui::SameLine();

    ImGui::BeginDisabled(value == 0);

    if (ImGui::SmallButton("-"))
    {
        --value;

        changed = true;
    }

    ImGui::EndDisabled();

    ImGui::SameLine();

    ImGui::Text("%u", value);

    ImGui::SameLine();

    ImGui::BeginDisabled(value >= maximum);

    if (ImGui::SmallButton("+"))
    {
        ++value;

        changed = true;
    }

    ImGui::EndDisabled();

    ImGui::PopID();

    return changed;
}

bool StepFloat(const char* label, float& value, float step, float minimum, float maximum, const char* format)
{
    const bool changed = ImGui::InputFloat(label, &value, step, step * 10.0f, format);

    if (changed)
    {
        value = std::clamp(value, minimum, maximum);
    }

    return changed;
}
} // namespace

void RuntimeDebugUI::BuildConfigReference()
{
    ImGui::TextWrapped(
        "All supported engine.cfg keys. Values below were loaded at startup; current live values are on the GI and Scene tabs. Files are not rewritten.");

    ImGui::InputTextWithHint("Search keys", "e.g. shadow, light.4, budget", m_configFilter, sizeof(m_configFilter));

    if (ImGui::BeginTable("Config keys", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                          ImVec2(0, 300)))
    {
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 2);

        ImGui::TableSetupColumn("Startup value", ImGuiTableColumnFlags_WidthStretch, 1);

        ImGui::TableSetupColumn("Control / scope", ImGuiTableColumnFlags_WidthStretch, 1);

        ImGui::TableSetupScrollFreeze(0, 1);

        ImGui::TableHeadersRow();

        for (const ConfigReference& entry : References)
        {
            ReferenceRow(entry.key, entry.fallback, entry.controls, m_configFilter);
        }

        // Show optional properties for every supported config slot, including inactive lights.
        for (uint32_t index = 0; index < rc::MaxSceneLights; ++index)
        {
            for (const ConfigReference& entry : LightReferences)
            {
                const std::string key = "light." + std::to_string(index) + "." + entry.key;

                ReferenceRow(key.c_str(), entry.fallback, entry.controls, m_configFilter);
            }
        }

        ImGui::EndTable();
    }
}

void RuntimeDebugUI::BuildSceneSettings()
{
    if (ImGui::CollapsingHeader("Camera"))
    {
        MarkSceneEdit(ImGui::DragFloat3("Position", &m_sceneDraft.cameraPosition.x, 0.01f));
    }

    if (ImGui::CollapsingHeader("Environment", ImGuiTreeNodeFlags_DefaultOpen))
    {
        MarkSceneEdit(ImGui::Checkbox("Environment lighting", &m_sceneDraft.environmentEnabled));

        MarkSceneEdit(ImGui::DragFloat("Intensity", &m_sceneDraft.environmentIntensity, 0.02f, 0, 10000, "%.2f",
                                       ImGuiSliderFlags_AlwaysClamp));

        MarkSceneEdit(ImGui::SliderFloat("Rotation (degrees)", &m_sceneDraft.environmentRotation, -180, 180, "%.1f",
                                         ImGuiSliderFlags_AlwaysClamp));

        MarkSceneEdit(ImGui::Checkbox("Skybox visible", &m_sceneDraft.skyboxVisible));
    }

    if (ImGui::CollapsingHeader("Lights", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (m_sceneDraft.boundsPresetLights)
        {
            ImGui::TextUnformatted("Preset lights around AABB");
        }

        if (m_sceneDraft.modelLightCount != 0)
        {
            ImGui::TextWrapped("The model supplies %u animated or authored lights.", m_sceneDraft.modelLightCount);
        }

        const uint32_t maximum = rc::MaxSceneLights - std::min(m_sceneDraft.modelLightCount, rc::MaxSceneLights);

        if (StepCount("Light count", m_sceneDraft.lightCount, maximum))
        {
            if (m_sceneDraft.animatedLight >= m_sceneDraft.lightCount)
            {
                m_sceneDraft.animationEnabled = false;

                m_sceneDraft.animatedLight    = m_sceneDraft.lightCount == 0 ? 0 : m_sceneDraft.lightCount - 1;
            }

            MarkSceneEdit(true);
        }

        MarkSceneEdit(ImGui::Checkbox("Light markers", &m_sceneDraft.markersEnabled));

        MarkSceneEdit(StepFloat("Marker size", m_sceneDraft.markerSize, 0.001f, 0.001f, 10, "%.3f"));

        for (uint32_t index = 0; index < m_sceneDraft.lightCount; ++index)
        {
            ImGui::PushID(static_cast<int>(index));

            const char* presetFaces[] = {"-X", "+X", "-Y", "+Y", "-Z", "+Z"};

            const bool preset         = m_sceneDraft.boundsPresetLights && index < 6;

            const bool open           = preset ? ImGui::TreeNode("Light", "Light %u (AABB %s)", index, presetFaces[index])
                                               : ImGui::TreeNode("Light", "Light %u", index);

            if (open)
            {
                rc::SceneLight& light = m_sceneDraft.lights[index];

                int type              = static_cast<int>(light.type);

                if (ImGui::Combo("Type", &type, "Directional\0Point\0Spot\0"))
                {
                    light.type = static_cast<rc::SceneLightType>(type);

                    if (light.type == rc::SceneLightType::eDirectional && index == m_sceneDraft.animatedLight)
                    {
                        m_sceneDraft.animationEnabled = false;
                    }

                    MarkSceneEdit(true);
                }

                MarkSceneEdit(ImGui::Checkbox("Enabled", &light.enabled));

                MarkSceneEdit(ImGui::Checkbox("Casts shadows", &light.castsShadows));

                const bool animated = m_sceneDraft.animationEnabled && index == m_sceneDraft.animatedLight;

                ImGui::BeginDisabled(animated);

                MarkSceneEdit(ImGui::DragFloat3("Position", &light.position.x, 0.01f));

                ImGui::EndDisabled();

                if (animated)
                {
                    ImGui::TextWrapped("Position follows the orbit. Disable animation to position this light manually.");
                }

                MarkSceneEdit(ImGui::DragFloat3("Direction", &light.direction.x, 0.01f));

                MarkSceneEdit(
                    ImGui::ColorEdit3("Color (linear)", &light.color.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR));

                MarkSceneEdit(StepFloat("Intensity", light.intensity, 0.1f, 0, 100000, "%.2f"));

                MarkSceneEdit(StepFloat("Range", light.range, 0.1f, 0, 100000, "%.3f"));

                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("0 means unlimited range.");
                }

                MarkSceneEdit(
                    StepFloat("Inner spot angle", light.innerAngleDegrees, 1, 0, light.outerAngleDegrees - 0.01f, "%.2f"));

                MarkSceneEdit(
                    StepFloat("Outer spot angle", light.outerAngleDegrees, 1, light.innerAngleDegrees + 0.01f, 89.99f, "%.2f"));

                ImGui::TreePop();
            }

            ImGui::PopID();
        }
    }

    if (ImGui::CollapsingHeader("Light animation"))
    {
        ImGui::BeginDisabled(m_sceneDraft.lightCount == 0);

        MarkSceneEdit(ImGui::Checkbox("Animate light", &m_sceneDraft.animationEnabled));

        const uint32_t maximum = m_sceneDraft.lightCount == 0 ? 0 : m_sceneDraft.lightCount - 1;

        MarkSceneEdit(StepCount("Light index", m_sceneDraft.animatedLight, maximum));

        ImGui::EndDisabled();

        MarkSceneEdit(ImGui::DragFloat3("Orbit center", &m_sceneDraft.orbitCenter.x, 0.01f));

        MarkSceneEdit(StepFloat("Orbit radius", m_sceneDraft.orbitRadius, 0.1f, 0, 10000, "%.3f"));

        MarkSceneEdit(StepFloat("Angular speed (deg/s)", m_sceneDraft.orbitSpeed, 5, -3600, 3600, "%.1f"));

        ImGui::TextWrapped("Animation requires an existing point or spot light.");
    }

    if (ImGui::CollapsingHeader("Assets (restart required)"))
    {
        const platform::ConfigLoader& config = platform::ConfigLoader::GetInstance();

        for (const ConfigReference& entry : References)
        {
            if (std::strstr(entry.controls, "restart required") != nullptr
                || std::strstr(entry.controls, "Restart required") != nullptr)
            {
                const std::string value = config.GetString(entry.key, entry.fallback);

                ImGui::TextWrapped("%s: %s", entry.key, value.c_str());
            }
        }

        ImGui::TextWrapped("Edit these paths in engine.cfg and restart to load different assets.");
    }
}
} // namespace zen::ui
