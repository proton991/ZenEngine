#include "Panels/EditorPanels.h"
#include "EditorWidgets.h"

namespace zen::editor
{
namespace
{
// Stack property labels in narrow docks; keep each widget's ID stable while resizing.
std::string PropertyLabel(const char* label)
{
    const bool stacked = ImGui::GetContentRegionAvail().x < 340 * EditorScale();

    std::string result = std::string("###") + label;

    if (stacked)
    {
        ImGui::TextWrapped("%s", label);

        ImGui::SetNextItemWidth(EditorControlWidth(200));
    }
    else
    {
        result.insert(0, label);
    }

    return result;
}

class RenderSettingsPanel final : public EditorPanel
{
public:
    RenderSettingsPanel() : EditorPanel({"RenderSettings", "Rendering", EditorDockArea::Right}) {}

private:
    void DrawEnvironment(EditorContext& context)
    {
        EditorController& editor = context.editor;

        EditorViewport& viewport = editor.GetViewport();

        if (ImGui::CollapsingHeader("Environment"))
        {
            ImGui::BeginDisabled(!viewport.HasScene() || editor.GetLoadState().IsActive() || editor.IsEnvironmentPending());

            rc::RenderingSettings draft = editor.GetRenderingState().GetDraft();

            EditorEnvironment& settings = draft.environment;

            const std::string label     = EnvironmentTextureLabel(settings.texturePath);

            ImGui::SetNextItemWidth(EditorControlWidth(320));

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
                SameLineIfFits(ImGui::CalcTextSize("Refresh list").x + ImGui::GetStyle().FramePadding.x * 2);
            }

            if (ImGui::Button("Refresh list"))
            {
                editor.RefreshEnvironmentTextures();
            }

            if (!context.nativeFileDialog)
            {
                ImGui::InputTextWithHint("##EnvironmentPath", "HDR / KTX / DDS file path", m_environmentPath,
                                         sizeof(m_environmentPath));

                SameLineIfFits(ImGui::CalcTextSize("Load").x + ImGui::GetStyle().FramePadding.x * 2);

                if (ImGui::Button("Load") && m_environmentPath[0] != '\0')
                {
                    editor.RequestEnvironmentTexture(m_environmentPath);
                }
            }

            bool changed = ImGui::DragFloat(PropertyLabel("Intensity").c_str(), &settings.intensity, 0.05f, 0.0f, 0.0f, "%.2f");

            if (ImGui::Button("Reset environment"))
            {
                const std::string texture = settings.texturePath;

                settings                  = editor.GetRenderingDefaults().environment;

                settings.texturePath      = texture;

                editor.RequestEnvironmentTexture("");

                changed = true;
            }

            changed |=
                ImGui::SliderFloat(PropertyLabel("Rotation").c_str(), &settings.rotationDegrees, -180.0f, 180.0f, "%.0f deg");

            changed |= ImGui::Checkbox("Environment lighting", &settings.lighting);

            changed |= ImGui::Checkbox("Show skybox", &settings.skybox);

            if (changed)
            {
                editor.StageRenderingSettings(draft);
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

    void DrawLights(EditorController& editor)
    {
        if (ImGui::CollapsingHeader("Lighting", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::BeginDisabled(!editor.GetViewport().HasScene() || editor.GetLoadState().IsActive());

            const char* types[] = {"Directional", "Point", "Spot"};

            ImGui::Combo(PropertyLabel("New type").c_str(), &m_newLightType, types, 3);

            if (ImGui::Button("Add light"))
            {
                editor.AddRenderingLight(static_cast<rc::SceneLightType>(m_newLightType));
            }

            SameLineIfFits(ImGui::CalcTextSize("Sides (6)").x + ImGui::GetStyle().FramePadding.x * 2);

            if (ImGui::Button("Sides (6)"))
            {
                editor.AddBoundsLights(false);
            }

            SameLineIfFits(ImGui::CalcTextSize("Corners (8)").x + ImGui::GetStyle().FramePadding.x * 2);

            if (ImGui::Button("Corners (8)"))
            {
                editor.AddBoundsLights(true);
            }

            if (ImGui::Button("Reset lights to glTF"))
            {
                editor.ResetRenderingLights();
            }

            SameLineIfFits(ImGui::CalcTextSize("Clear lights").x + ImGui::GetStyle().FramePadding.x * 2);

            rc::RenderingSettings draft = editor.GetRenderingState().GetDraft();

            bool changed                = false;

            if (ImGui::Button("Clear lights"))
            {
                draft.lights.clear();

                changed = true;
            }

            if (draft.lights.empty())
            {
                ImGui::TextUnformatted("No lights");
            }

            const char* origins[] = {"glTF", "Manual", "Side preset", "Corner preset"};

            size_t remove         = draft.lights.size();

            for (size_t index = 0; index < draft.lights.size(); ++index)
            {
                rc::RenderingLight& entry = draft.lights[index];

                ImGui::PushID(static_cast<int>(entry.id));

                const std::string label = std::string(origins[static_cast<int>(entry.origin)]) + " " + std::to_string(entry.id);

                if (ImGui::TreeNode(label.c_str()))
                {
                    rc::SceneLight& light  = entry.light;

                    int type               = static_cast<int>(light.type);

                    changed               |= ImGui::Checkbox("Enabled", &light.enabled);

                    changed               |= ImGui::Combo(PropertyLabel("Type").c_str(), &type, types, 3);

                    light.type             = static_cast<rc::SceneLightType>(type);

                    changed |= ImGui::ColorEdit3(PropertyLabel("Color").c_str(), &light.color.x, ImGuiColorEditFlags_Float);

                    changed |= ImGui::DragFloat(PropertyLabel("Intensity").c_str(), &light.intensity, 0.01f);

                    if (light.type != rc::SceneLightType::eDirectional)
                    {
                        changed |= ImGui::DragFloat3(PropertyLabel("Position").c_str(), &light.position.x, 0.01f);

                        changed |= ImGui::DragFloat(PropertyLabel("Range").c_str(), &light.range, 0.01f);
                    }

                    if (light.type != rc::SceneLightType::ePoint)
                    {
                        changed |= ImGui::DragFloat3(PropertyLabel("Direction").c_str(), &light.direction.x, 0.01f);
                    }

                    if (light.type == rc::SceneLightType::eSpot)
                    {
                        changed |= ImGui::DragFloat(PropertyLabel("Inner angle").c_str(), &light.innerAngleDegrees, 0.1f);

                        changed |= ImGui::DragFloat(PropertyLabel("Outer angle").c_str(), &light.outerAngleDegrees, 0.1f);
                    }

                    changed |= ImGui::Checkbox("Casts shadows (GI path)", &light.castsShadows);

                    if (ImGui::Button("Remove"))
                    {
                        remove = index;
                    }

                    ImGui::TreePop();
                }

                ImGui::PopID();
            }

            if (remove < draft.lights.size())
            {
                draft.lights.erase(draft.lights.begin() + remove);

                changed = true;
            }

            changed |= ImGui::Checkbox("Light markers", &draft.lightMarkers);

            changed |= ImGui::DragFloat(PropertyLabel("Marker size").c_str(), &draft.lightMarkerSize, 0.001f);

            ImGui::TextWrapped("Enable Move lights in the Scene toolbar to drag point and spot lights in the viewport.");

            ImGui::SeparatorText("Light ball - GI test");

            changed     |= ImGui::Checkbox("Light ball", &draft.cameraLight.enabled);

            bool follow  = draft.cameraLight.followCamera;

            if (ImGui::Checkbox("Follow camera", &follow))
            {
                Vec3 origin;

                Vec3 direction;

                editor.GetCamera().MakeRay(Vec2(0.5f), origin, direction);

                draft.cameraLight.position =
                    rc::CameraLightPosition(draft.cameraLight, editor.GetCamera().GetCamera().GetPos(), direction);

                draft.cameraLight.followCamera = follow;

                changed                        = true;
            }

            if (follow)
            {
                changed |= ImGui::DragFloat(PropertyLabel("Hold distance").c_str(), &draft.cameraLight.followDistance, 0.001f,
                                            0, 0, "%.3f");
            }
            else
            {
                changed |= ImGui::DragFloat3(PropertyLabel("Ball position").c_str(), &draft.cameraLight.position.x, 0.001f, 0,
                                             0, "%.3f");
            }

            changed |=
                ImGui::ColorEdit3(PropertyLabel("Ball color").c_str(), &draft.cameraLight.color.x, ImGuiColorEditFlags_Float);

            changed |=
                ImGui::DragFloat(PropertyLabel("Ball intensity").c_str(), &draft.cameraLight.intensity, 0.0001f, 0, 0, "%.4f");

            changed |= ImGui::DragFloat(PropertyLabel("Light range").c_str(), &draft.cameraLight.range, 0.001f, 0, 0, "%.3f");

            changed |= ImGui::DragFloat(PropertyLabel("Ball radius").c_str(), &draft.cameraLight.radius, 0.0001f, 0, 0, "%.4f");

            if (ImGui::Button("Reset light ball"))
            {
                draft.cameraLight = editor.GetRenderingDefaults().cameraLight;

                changed           = true;
            }

            ImGui::TextWrapped(
                "Disable Follow camera to hold the ball in place. Range, radius and distance use normalized scene units (scene span = 1). Range and hold distance must exceed the radius.");

            ImGui::TextWrapped(
                "Lights nearby surfaces in every direction. Select PBR + voxel GI and enable Analytic lighting for bounce light. Mesh shadows include the ball; moving it updates shadows and GI radiance.");

            if (changed)
            {
                editor.StageRenderingSettings(draft);
            }

            ImGui::EndDisabled();
        }
    }

    void DrawGI(EditorController& editor, const EditorRenderSnapshot& snapshot)
    {
        rc::RenderingSettings draft          = editor.GetRenderingState().GetDraft();

        rc::VoxelGIRuntimeSettings& settings = draft.gi;

        bool changed                         = false;

        if (ImGui::CollapsingHeader("Shadows"))
        {
            if (ImGui::Button("Reset shadows"))
            {
                settings.shadowMapResolution = editor.GetRenderingDefaults().gi.shadowMapResolution;

                settings.cone.shadows        = editor.GetRenderingDefaults().gi.cone.shadows;

                changed                      = true;
            }

            ImGui::TextWrapped(
                "Mesh shadows affect direct lights in PBR + voxel GI and update immediately. Environment lighting and voxel occlusion are independent.");

            changed                      |= ImGui::Checkbox("Mesh shadows", &settings.cone.shadows);

            int resolution                = static_cast<int>(settings.shadowMapResolution);

            changed                      |= ImGui::InputInt(PropertyLabel("Map size").c_str(), &resolution);

            settings.shadowMapResolution  = static_cast<uint32_t>(resolution);

            ImGui::TextWrapped("Map size changes require Apply.");

            HeapVector<rc::SceneLight> lights;

            for (const rc::RenderingLight& light : draft.lights)
            {
                lights.push_back(light.light);
            }

            const uint32_t faces = rc::CountShadowFaces(lights) + rc::CameraLightShadowFaces(draft.cameraLight);

            ImGui::Text("%u faces | estimated array %.1f MiB", faces,
                        double(rc::EstimateShadowBytes(settings.shadowMapResolution, faces)) / (1024.0 * 1024.0));

            if (faces == 0)
            {
                ImGui::TextWrapped(
                    "No shadow-casting lights. Add one under Lighting or enable Light ball to see mesh shadows.");
            }
        }

        if (ImGui::CollapsingHeader("GI"))
        {
            if (ImGui::Button("Reset GI"))
            {
                settings.cone = editor.GetRenderingDefaults().gi.cone;

                changed       = true;
            }

            rc::VoxelGISettings& cone  = settings.cone;

            int count                  = cone.coneCount == 4 ? 0 : 1;

            const char* counts[]       = {"4", "6"};

            changed                   |= ImGui::Combo(PropertyLabel("Cones").c_str(), &count, counts, 2);

            cone.coneCount             = count == 0 ? 4 : 6;

            int steps                  = static_cast<int>(cone.maxSteps);

            changed                   |= ImGui::SliderInt(PropertyLabel("Max steps").c_str(), &steps, 8, 512);

            cone.maxSteps              = static_cast<uint32_t>(steps);

            changed |= ImGui::SliderFloat(PropertyLabel("Cone angle").c_str(), &cone.coneAngleDegrees, 10.0f, 90.0f);

            changed |= ImGui::SliderFloat(PropertyLabel("Indirect intensity").c_str(), &cone.indirectIntensity, 0.0f, 10.0f);

            changed |= ImGui::SliderFloat(PropertyLabel("Step scale").c_str(), &cone.stepScale, 0.25f, 2.0f);

            changed |= ImGui::SliderFloat(PropertyLabel("Bias (voxels)").c_str(), &cone.normalBiasVoxels, 0.5f, 4.0f);

            changed |= ImGui::SliderFloat(PropertyLabel("GI distance").c_str(), &cone.maxDistanceGridLengths, 0.01f, 2.0f);

            ImGui::TextDisabled("GI distance is measured in grid lengths.");

            ImGui::TextUnformatted("Indirect lighting contributions:");

            changed |= ImGui::Checkbox("Analytic lights into GI", &cone.analyticLighting);

            changed |= ImGui::Checkbox("Environment diffuse GI", &cone.environmentLighting);

            changed |= ImGui::Checkbox("Emissive into GI", &cone.emissiveLighting);

            ImGui::TextWrapped("These toggles do not disable direct lighting, specular environment lighting or the skybox.");
        }

        if (ImGui::CollapsingHeader("GI resources"))
        {
            if (ImGui::Button("Reset GI resources"))
            {
                const rc::VoxelGISettings cone = settings.cone;

                const uint32_t shadows         = settings.shadowMapResolution;

                settings                       = editor.GetRenderingDefaults().gi;

                settings.cone                  = cone;

                settings.shadowMapResolution   = shadows;

                changed                        = true;
            }

            int resolution                   = settings.resolution == 64 ? 0 : settings.resolution == 128 ? 1 : 2;

            const char* resolutions[]        = {"64", "128", "256"};

            changed                         |= ImGui::Combo(PropertyLabel("Grid size").c_str(), &resolution, resolutions, 3);

            settings.resolution              = 64u << resolution;

            int voxelizer                    = settings.voxelizer == platform::VoxelizerMode::eAuto    ? 0
                                             : settings.voxelizer == platform::VoxelizerMode::eCompute ? 1
                                                                                                       : 2;

            const char* voxelizers[]         = {"Auto", "Compute", "Geometry"};

            changed                         |= ImGui::Combo(PropertyLabel("Voxelizer").c_str(), &voxelizer, voxelizers, 3);

            settings.voxelizer               = voxelizer == 0 ? platform::VoxelizerMode::eAuto
                                             : voxelizer == 1 ? platform::VoxelizerMode::eCompute
                                                              : platform::VoxelizerMode::eGeometry;

            changed                         |= ImGui::Checkbox("Averaged reflectance (Apply)", &settings.averagedReflectance);

            int budget                       = static_cast<int>(settings.reflectanceBudgetBytes / (1024 * 1024));

            changed                         |= ImGui::InputInt(PropertyLabel("Budget MiB").c_str(), &budget);

            settings.reflectanceBudgetBytes  = uint64_t(std::max(budget, 0)) * 1024 * 1024;

            bool async                       = settings.asyncCompute == platform::AsyncComputeMode::eAuto;

            changed                         |= ImGui::Checkbox("Prefer async compute", &async);

            settings.asyncCompute = async ? platform::AsyncComputeMode::eAuto : platform::AsyncComputeMode::eDisabled;

            ImGui::Text("Effective voxelizer: %s",
                        snapshot.effectiveVoxelizer == platform::VoxelizerMode::eGeometry ? "Geometry" : "Compute");

            ImGui::Text("GPU committed: %.1f MiB", double(snapshot.memory.deviceLocalBytes) / (1024.0 * 1024.0));

            ImGui::TextWrapped(
                "Apply validates resources before rebuilding. A later GPU allocation failure is reported as a fallback.");
        }

        if (changed)
        {
            editor.StageRenderingSettings(draft);
        }
    }

    void DrawDebug(EditorController& editor, const EditorRenderSnapshot& snapshot)
    {
        if (ImGui::CollapsingHeader("Debug output", ImGuiTreeNodeFlags_DefaultOpen))
        {
            rc::RenderingSettings draft = editor.GetRenderingState().GetDraft();

            rc::DebugSelection& debug   = draft.debug;

            int output                  = static_cast<int>(debug.output);

            const char* outputs[] = {"Final", "Depth", "Albedo", "Normal (world)", "Shadow", "Voxels (3D)", "Voxel slice"};

            bool changed          = ImGui::Combo(PropertyLabel("Output").c_str(), &output, outputs, 7);

            if (changed)
            {
                debug.output  = static_cast<rc::DebugOutput>(output);

                debug.minimum = 0.0f;

                debug.maximum = debug.output == rc::DebugOutput::eDepth && debug.linearDepth ? 10.0f : 1.0f;

                debug.slice   = std::min(debug.slice, draft.gi.resolution - 1);

                if (debug.lightId == 0 && !draft.lights.empty())
                {
                    debug.lightId = draft.lights.front().id;
                }
            }

            if (debug.output == rc::DebugOutput::eDepth)
            {
                if (ImGui::Checkbox("Linear view distance", &debug.linearDepth))
                {
                    debug.maximum = debug.linearDepth ? 10.0f : 1.0f;

                    changed       = true;
                }
            }

            if (debug.output == rc::DebugOutput::eShadow)
            {
                const std::string selected = debug.lightId == 0 ? "Select light" : std::to_string(debug.lightId);

                if (ImGui::BeginCombo("Shadow light", selected.c_str()))
                {
                    for (const rc::RenderingLight& light : draft.lights)
                    {
                        if (light.light.enabled && light.light.castsShadows && light.light.intensity > 0.0f)
                        {
                            const std::string label = std::to_string(light.id);

                            if (ImGui::Selectable(label.c_str(), light.id == debug.lightId))
                            {
                                debug.lightId = light.id;

                                debug.face    = 0;

                                changed       = true;
                            }
                        }
                    }

                    ImGui::EndCombo();
                }

                bool point = false;

                for (const rc::RenderingLight& light : draft.lights)
                {
                    point |= light.id == debug.lightId && light.light.type == rc::SceneLightType::ePoint;
                }

                if (point)
                {
                    int face             = static_cast<int>(debug.face);

                    const char* faces[]  = {"+X", "-X", "+Y", "-Y", "+Z", "-Z"};

                    changed             |= ImGui::Combo(PropertyLabel("Face").c_str(), &face, faces, 6);

                    debug.face           = static_cast<uint32_t>(face);
                }
                else if (debug.face != 0)
                {
                    debug.face = 0;

                    changed    = true;
                }
            }

            if (debug.output == rc::DebugOutput::eDepth || debug.output == rc::DebugOutput::eShadow)
            {
                changed |= ImGui::DragFloat(PropertyLabel("Range minimum").c_str(), &debug.minimum, 0.001f);

                changed |= ImGui::DragFloat(PropertyLabel("Range maximum").c_str(), &debug.maximum, 0.001f);
            }

            if (debug.output == rc::DebugOutput::eVoxelSlice)
            {
                int axis            = static_cast<int>(debug.axis);

                const char* axes[]  = {"X", "Y", "Z"};

                changed            |= ImGui::Combo(PropertyLabel("Slice axis").c_str(), &axis, axes, 3);

                debug.axis          = static_cast<uint32_t>(axis);

                int slice           = static_cast<int>(debug.slice);

                changed |=
                    ImGui::SliderInt(PropertyLabel("Slice").c_str(), &slice, 0, static_cast<int>(draft.gi.resolution) - 1);

                debug.slice = static_cast<uint32_t>(slice);

                ImGui::TextDisabled("Surface albedo: mip 0 (only available mip)");
            }

            if (changed)
            {
                editor.StageRenderingSettings(draft);
            }

            if (snapshot.debug.available)
            {
                ImGui::Text("Source: %u x %u x %u", snapshot.debug.width, snapshot.debug.height, snapshot.debug.depth);

                ImGui::TextWrapped("%s", snapshot.debug.interpretation.c_str());
            }
            else
            {
                ImGui::TextWrapped("Unavailable: %s", snapshot.debug.reason.c_str());
            }
        }
    }

    void DrawSettings(EditorContext& context)
    {
        EditorController& editor = context.editor;

        ImGui::PushItemWidth(std::min(EditorControlWidth(200), ImGui::GetContentRegionAvail().x * 0.56f));

        ImGui::TextWrapped("Scene: %s", editor.GetRenderingState().GetDraft().scenePath.empty()
                                            ? "None"
                                            : editor.GetRenderingState().GetDraft().scenePath.c_str());

        if (ImGui::Button("Open scene..."))
        {
            editor.GetActions().Execute(actions::Open);
        }

        SameLineIfFits(ImGui::CalcTextSize("Reset setup").x + ImGui::GetStyle().FramePadding.x * 2);

        ImGui::BeginDisabled(editor.IsEnvironmentPending() || editor.GetLoadState().IsActive());

        if (ImGui::Button("Reset setup"))
        {
            editor.ResetRenderingSetup();
        }

        ImGui::EndDisabled();

        rc::RenderingSettings draft = editor.GetRenderingState().GetDraft();

        int algorithm               = static_cast<int>(draft.algorithm);

        const char* algorithms[]    = {"PBR", "PBR + voxel cone GI"};

        if (ImGui::Combo(PropertyLabel("Algorithm").c_str(), &algorithm, algorithms, 2))
        {
            draft.algorithm    = static_cast<rc::RenderAlgorithm>(algorithm);

            draft.debug.output = rc::DebugOutput::eFinal;

            editor.StageRenderingSettings(draft);
        }

        int projection            = editor.GetCamera().IsOrthographic() ? 1 : 0;

        const char* projections[] = {"Perspective", "Orthographic"};

        if (ImGui::Combo(PropertyLabel("View projection").c_str(), &projection, projections, 2))
        {
            editor.GetCamera().SetOrthographic(projection == 1);
        }

        const EditorRenderingState& state = editor.GetRenderingState();

        ImGui::Text("Revision: %llu / applied: %llu", static_cast<unsigned long long>(state.GetRevision()),
                    static_cast<unsigned long long>(state.GetAppliedRevision()));

        if (!state.GetError().empty())
        {
            ImGui::TextWrapped("%s", state.GetError().c_str());
        }

        const EditorRenderSnapshot snapshot = context.editor.GetViewport().GetSnapshot();

        if (snapshot.debug.output == rc::DebugOutput::eFinal || snapshot.debug.output == rc::DebugOutput::eDepth)
        {
            ImGui::Text("Effective algorithm: %s",
                        snapshot.status.effective == rc::RenderAlgorithm::eVoxelGI ? "PBR + voxel GI" : "PBR");
        }
        else
        {
            ImGui::TextUnformatted("Inspecting diagnostic output");
        }

        DrawLights(editor);

        DrawEnvironment(context);

        DrawGI(editor, snapshot);

        DrawDebug(editor, snapshot);

        const rc::RenderView& view = context.editor.GetViewport().GetRenderView();

        ImGui::Text("Scene target: %u x %u", view.width, view.height);

        ImGui::TextUnformatted("SDR | single sample | independent editor camera");

        ImGui::TextWrapped(
            "Surface picking includes opaque and alpha-masked triangles. Blended/transmissive surfaces, points and lines are excluded. Animations are frozen.");

        if (snapshot.requestedMode != snapshot.renderedMode)
        {
            ImGui::TextWrapped("PBR fallback: %s", snapshot.status.fallbackReason.c_str());

            if (ImGui::Button("Retry settings"))
            {
                editor.StageRenderingSettings(editor.GetRenderingState().GetDraft(), true);
            }

            ImGui::BeginDisabled(!editor.CanRestoreRenderingSettings());

            if (ImGui::Button("Restore last working setup"))
            {
                editor.RestoreRenderingSettings();
            }

            ImGui::EndDisabled();
        }

        ImGui::PopItemWidth();
    }

    void DrawContents(EditorContext& context) override
    {
        const ImGuiStyle& style = ImGui::GetStyle();

        const float buttonsWidth =
            ImGui::CalcTextSize("Apply").x + ImGui::CalcTextSize("Revert").x + 4 * style.FramePadding.x + style.ItemSpacing.x;

        const float buttonRows = ImGui::GetContentRegionAvail().x >= buttonsWidth ? 1.0f : 2.0f;

        // Reserve the same footer height for pending and applied states, including wrapped buttons.
        const float footerHeight = ImGui::GetTextLineHeightWithSpacing() + buttonRows * ImGui::GetFrameHeightWithSpacing()
                                 + 2 * style.ItemSpacing.y + 1;

        const float settingsHeight = std::max(1.0f, ImGui::GetContentRegionAvail().y - footerHeight);

        if (ImGui::BeginChild("RenderingSettingsScroll", ImVec2(0, settingsHeight), ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoBackground))
        {
            DrawSettings(context);
        }

        ImGui::EndChild();

        ImGui::Separator();

        EditorController& editor          = context.editor;

        const EditorRenderingState& state = editor.GetRenderingState();

        const bool pending                = state.IsPending();

        ImGui::TextDisabled("%s", !pending                     ? "All changes applied"
                                  : state.NeedsResourceApply() ? "Resource changes pending Apply"
                                                               : "Changes pending");

        ImGui::BeginDisabled(!pending);

        if (ImGui::Button("Apply"))
        {
            editor.StageRenderingSettings(state.GetDraft(), true);
        }

        SameLineIfFits(ImGui::CalcTextSize("Revert").x + style.FramePadding.x * 2);

        if (ImGui::Button("Revert"))
        {
            editor.RevertRenderingSettings();
        }

        ImGui::EndDisabled();
    }

    int m_newLightType{1};

    char m_environmentPath[4096]{};
};
} // namespace

UniquePtr<EditorPanel> CreateRenderSettingsPanel()
{
    return MakeUnique<RenderSettingsPanel>();
}
} // namespace zen::editor
