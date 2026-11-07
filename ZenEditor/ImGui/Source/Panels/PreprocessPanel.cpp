#include "Panels/EditorPanels.h"
#include "EditorWidgets.h"
#include "misc/cpp/imgui_stdlib.h"

namespace zen::editor
{
namespace
{
class PreprocessPanel final : public EditorPanel
{
public:
    PreprocessPanel() : EditorPanel({"Preprocess", "Preprocess Models", EditorDockArea::Center, false}) {}

private:
    void DrawContents(EditorContext& context) override
    {
        AssetPreprocessor& build = context.editor.GetPreprocessor();
        ImGui::TextWrapped(
            "Prepare glTF, GLB, FBX or OBJ models for ZenEngine. A native scene references reusable model packages; sources are not required to open it.");
        ImGui::BeginDisabled(build.IsRunning());
        ImGui::TextUnformatted("Model path");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##ModelPath", &m_source);
        if (ImGui::Button("Add model") && build.AddSource(m_source))
        {
            m_source.clear();
        }
        if (context.nativeFileDialog)
        {
            ImGui::SameLine();
            if (ImGui::Button("Browse model..."))
            {
                build.browseRequested = true;
            }
        }
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Add one model or several. Each model keeps its authored coordinates.");
        ImGui::PopTextWrapPos();
        for (size_t index = 0; index < build.sources.size();)
        {
            ImGui::PushID(static_cast<int>(index));
            const bool remove = ImGui::SmallButton("Remove");
            ImGui::SameLine();
            ImGui::TextWrapped("%s", build.sources[index].c_str());
            ImGui::PopID();
            if (remove)
            {
                build.sources.erase(build.sources.begin() + index);
            }
            else
            {
                ++index;
            }
        }
        ImGui::Separator();
        ImGui::TextUnformatted("Output folder");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##OutputFolder", &build.outputDirectory);
        if (context.nativeFileDialog && ImGui::Button("Browse folder..."))
        {
            build.browseOutputRequested = true;
        }
        ImGui::TextUnformatted("Scene name");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##SceneName", &build.name);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("The output is <scene name>.zenscene and a models folder. Keep both together.");
        ImGui::PopTextWrapPos();
        ImGui::Checkbox("Compress textures with quality checks", &build.compressTextures);
        ImGui::TextWrapped(
            build.compressTextures
                ? "Full-resolution BC textures and mipmaps where quality checks pass. Ineligible textures retain their original pixels."
                : "Lossless storage and vertex-fetch optimization. Authored pixels, geometry and materials are preserved.");
        ImGui::SetNextItemWidth(90.0f);
        ImGui::InputScalar("Import workers", ImGuiDataType_U32, &build.workers);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("Maximum CPU threads used to import a model (1-64).\n"
                              "More workers can speed up decoding, but use more CPU and memory.\n"
                              "Models are processed one at a time; some import stages remain serial.");
        }
        if (ImGui::Button("Preprocess models"))
        {
            build.Start();
        }
        ImGui::EndDisabled();
        if (build.IsRunning())
        {
            ImGui::SameLine();
            if (ImGui::Button("Cancel processing"))
            {
                build.Cancel();
            }
        }
        if (!build.GetProgressText().empty())
        {
            const std::string percent = std::to_string(static_cast<uint32_t>(build.GetProgress() * 100.0f)) + "%";
            ImGui::ProgressBar(build.GetProgress(), ImVec2(-1.0f, 0.0f), percent.c_str());
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Progress follows completed build stages, not an estimate of remaining time.");
            }
            ImGui::TextWrapped("%s", build.GetProgressText().c_str());
        }
        ImGui::TextWrapped("%s", build.GetStatusText().c_str());
        if (build.Succeeded())
        {
            ImGui::TextWrapped("%s", build.GetOutput().c_str());
            ImGui::BeginDisabled(context.editor.GetLoadState().IsActive() || context.editor.IsPreviewFrozen());
            if (ImGui::Button("Open processed scene"))
            {
                context.editor.RequestLoad(build.GetOutput());
            }
            ImGui::EndDisabled();
        }
        if (!build.GetLog().empty() && ImGui::CollapsingHeader("Compiler log"))
        {
            ImGui::TextUnformatted(build.GetLog().c_str());
        }
    }
    std::string m_source;
};
} // namespace
UniquePtr<EditorPanel> CreatePreprocessPanel()
{
    return MakeUnique<PreprocessPanel>();
}
} // namespace zen::editor
