#include "SceneProcessingWidget.h"
#include "imgui.h"

namespace zen::editor
{
bool DrawSceneProcessingSettings(SceneProcessingSettings& settings)
{
    int  profile     = settings.desktop ? 1 : 0;
    bool changed     = ImGui::Combo("Profile", &profile, "Reference\0Desktop (explicit options)\0");
    settings.desktop = profile == 1;
    ImGui::TextWrapped("Applies to the next source open or explicit recook. Packages load as stored.");
    ImGui::BeginDisabled(!settings.desktop);
    zenasset::GeometrySettings& geometry  = settings.settings.geometry;
    zenasset::TextureSettings&  textures  = settings.settings.textures;
    changed                              |= ImGui::Checkbox("Optimize vertex fetch", &geometry.optimizeVertexFetch);
    changed                              |= ImGui::Checkbox("Generate texture mips", &textures.generateMipmaps);
    changed                              |= ImGui::Checkbox("BC7 color/data textures", &textures.compressBC7);
    changed                              |= ImGui::Checkbox("BC5 eligible normals", &textures.compressBC5Normals);
    changed                              |= ImGui::Checkbox("BC4 occlusion", &textures.compressBC4Occlusion);
    changed                              |= ImGui::Checkbox("BC6H environment", &textures.compressBC6HEnvironment);
    ImGui::TextWrapped(
        "Compression is opt-in. Images outside the quality or compatibility limits retain their original pixels.");
    if (ImGui::CollapsingHeader("Advanced processing"))
    {
        changed |= ImGui::InputScalar("Position bits (0 disables)", ImGuiDataType_U32, &geometry.positionQuantizationBits);
        changed |= ImGui::InputFloat("Position relative error", &geometry.positionMaxRelativeError, 0, 0, "%.7g");
        changed |= ImGui::InputFloat("Position absolute error", &geometry.positionMaxAbsoluteError, 0, 0, "%.7g");
        changed |= ImGui::InputScalar("BC7 quality (0-4)", ImGuiDataType_U32, &textures.bc7Quality);
        changed |= ImGui::InputScalar("Maximum image dimension", ImGuiDataType_U32, &textures.maxDimension);
        changed |= ImGui::InputFloat("Color minimum PSNR", &textures.colorMinPsnr, 0, 0, "%.7g");
        changed |= ImGui::InputFloat("Color maximum RMSE", &textures.colorMaxRmse, 0, 0, "%.7g");
        changed |= ImGui::InputFloat("Scalar maximum error", &textures.scalarMaxError, 0, 0, "%.7g");
        changed |= ImGui::InputFloat("Normal RMS degrees", &textures.normalRmsAngleDegrees, 0, 0, "%.7g");
        changed |= ImGui::InputFloat("Normal maximum degrees", &textures.normalMaxAngleDegrees, 0, 0, "%.7g");
        changed |= ImGui::InputFloat("Alpha coverage error", &textures.alphaCoverageMaxError, 0, 0, "%.7g");
        changed |= ImGui::InputFloat("HDR relative RMSE", &textures.hdrMaxRelativeRmse, 0, 0, "%.7g");
    }
    ImGui::EndDisabled();
    return changed;
}
} // namespace zen::editor
