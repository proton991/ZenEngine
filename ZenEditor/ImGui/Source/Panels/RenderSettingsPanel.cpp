#include "Panels/EditorPanels.h"
#include "EditorWidgets.h"

namespace zen::editor
{
namespace
{
class RenderSettingsPanel final : public EditorPanel
{
public:
    RenderSettingsPanel() : EditorPanel({"RenderSettings", "Render Settings", EditorDockArea::Right}) {}

private:
    void DrawEnvironment(EditorContext& context)
    {
        EditorController& editor = context.editor;

        EditorViewport& viewport = editor.GetViewport();

        if (ImGui::CollapsingHeader("Environment", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::BeginDisabled(!viewport.HasScene() || editor.GetLoadState().IsActive() || editor.IsEnvironmentPending());

            EditorEnvironment settings = viewport.GetEnvironment();

            const std::string label    = EnvironmentTextureLabel(settings.texturePath);

            ImGui::SetNextItemWidth(-1);

            if (ImGui::BeginCombo("##EnvironmentTexture", label.c_str()))
            {
                if (ImGui::Selectable("Scene / engine default", settings.texturePath.empty()))
                {
                    editor.RequestEnvironmentTexture("");
                }

                for (const EnvironmentTextureItem& item : editor.GetEnvironmentTextures())
                {
                    ImGui::PushID(item.path.c_str());

                    if (ImGui::Selectable(item.label.c_str(), settings.texturePath == item.path))
                    {
                        editor.RequestEnvironmentTexture(item.path);
                    }

                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("%s", item.path.c_str());
                    }

                    ImGui::PopID();
                }

                ImGui::EndCombo();
            }

            if (context.nativeFileDialog && ImGui::Button("Browse..."))
            {
                editor.RequestEnvironmentFileOpen();
            }

            if (context.nativeFileDialog)
            {
                ImGui::SameLine();
            }

            if (ImGui::Button("Refresh list"))
            {
                editor.RefreshEnvironmentTextures();
            }

            if (!context.nativeFileDialog)
            {
                ImGui::InputTextWithHint("##EnvironmentPath", "HDR / KTX / DDS file path", m_environmentPath,
                                         sizeof(m_environmentPath));

                ImGui::SameLine();

                if (ImGui::Button("Load") && m_environmentPath[0] != '\0')
                {
                    editor.RequestEnvironmentTexture(m_environmentPath);
                }
            }

            bool changed =
                ImGui::DragFloat("Intensity", &settings.intensity, 0.05f, 0.0f, 100.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);

            changed |= ImGui::SliderFloat("Rotation", &settings.rotationDegrees, -180.0f, 180.0f, "%.0f deg");

            changed |= ImGui::Checkbox("Environment lighting", &settings.lighting);

            changed |= ImGui::Checkbox("Show skybox", &settings.skybox);

            if (changed)
            {
                viewport.SetEnvironmentLighting(settings.intensity, settings.rotationDegrees, settings.lighting,
                                                settings.skybox);
            }

            ImGui::EndDisabled();

            if (!viewport.HasScene() && !settings.texturePath.empty())
            {
                ImGui::BeginDisabled(editor.GetLoadState().IsActive() || editor.IsEnvironmentPending());

                if (ImGui::Button("Restore scene / engine default"))
                {
                    editor.RequestEnvironmentTexture("");
                }

                ImGui::EndDisabled();
            }

            if (editor.IsEnvironmentPending())
            {
                ImGui::TextUnformatted("Loading environment...");
            }

            if (!editor.GetEnvironmentError().empty())
            {
                ImGui::TextWrapped("%s", editor.GetEnvironmentError().c_str());
            }

            ImGui::TextDisabled("HDR panorama or floating-point cubemap");

            ImGui::TextWrapped("Preview settings apply across scenes for this session.");
        }
    }

    void DrawContents(EditorContext& context) override
    {
        DrawEnvironment(context);

        const EditorRenderSnapshot snapshot        = context.editor.GetViewport().GetSnapshot();

        const rc::VoxelGIRuntimeSettings& settings = snapshot.settings;

        ImGui::TextDisabled("Renderer configuration (read-only)");

        ImGui::Separator();

        ImGui::Text("Voxel resolution: %u", settings.resolution);

        ImGui::Text("Shadow map: %u", settings.shadowMapResolution);

        if (ImGui::CollapsingHeader("Cone GI"))
        {
            const rc::VoxelGISettings& cone = settings.cone;

            ImGui::TextWrapped("Cones: %u | Angle: %.1f degrees | Max steps: %u", cone.coneCount, cone.coneAngleDegrees,
                               cone.maxSteps);

            ImGui::TextWrapped("Indirect intensity: %.3f | Step scale: %.3f", cone.indirectIntensity, cone.stepScale);

            ImGui::TextWrapped("Normal bias: %.3f voxels | Distance: %.3f grid lengths", cone.normalBiasVoxels,
                               cone.maxDistanceGridLengths);

            ImGui::TextWrapped("Shadows: %s | Analytic: %s | Environment: %s | Emissive: %s", cone.shadows ? "On" : "Off",
                               cone.analyticLighting ? "On" : "Off", cone.environmentLighting ? "On" : "Off",
                               cone.emissiveLighting ? "On" : "Off");
        }

        ImGui::Text("Reflectance: %s", settings.averagedReflectance ? "Averaged" : "Owner");

        ImGui::Text("Async compute: %s", settings.asyncCompute == platform::AsyncComputeMode::eAuto ? "Auto" : "Disabled");

        const rc::RenderView& view = context.editor.GetViewport().GetRenderView();

        ImGui::Text("Scene target: %u x %u", view.width, view.height);

        ImGui::TextUnformatted("SDR | single sample | independent editor camera");

        ImGui::TextWrapped(
            "Surface picking includes opaque and alpha-masked triangles. Blended/transmissive surfaces, points and lines are excluded. Animations are frozen.");

        if (snapshot.requestedMode != snapshot.renderedMode)
        {
            ImGui::TextWrapped("Rendering fell back to PBR. See Output for resource or coverage diagnostics.");
        }
    }

    char m_environmentPath[4096]{};
};
} // namespace

UniquePtr<EditorPanel> CreateRenderSettingsPanel()
{
    return MakeUnique<RenderSettingsPanel>();
}
} // namespace zen::editor
