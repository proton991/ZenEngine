#include "RuntimeUI/RuntimeDebugUI.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelizerBase.h"
#include "imgui.h"
#include <algorithm>

namespace zen::ui
{
namespace
{
bool UIntSlider(const char* label, uint32_t& value, uint32_t minimum, uint32_t maximum)
{
    return ImGui::SliderScalar(label, ImGuiDataType_U32, &value, &minimum, &maximum, "%u", ImGuiSliderFlags_AlwaysClamp);
}

bool BudgetInput(const char* label, uint64_t& bytes)
{
    const uint64_t bytesPerMiB = 1024 * 1024;

    const uint64_t step        = 64;

    uint64_t mib               = bytes / bytesPerMiB;

    const bool changed         = ImGui::InputScalar(label, ImGuiDataType_U64, &mib, &step);

    if (changed)
    {
        bytes = std::min(mib, UINT64_MAX / bytesPerMiB) * bytesPerMiB;
    }

    return changed;
}

const char* ResourceValidationMessage(rc::GIResourceStatus status)
{
    const char* message = "Invalid GI settings. Check numeric ranges before applying.";

    switch (status)
    {
        case rc::GIResourceStatus::eBudget:
            message = "Reflectance budget is below the required minimum. Current settings are unchanged.";
            break;
        case rc::GIResourceStatus::eDescriptorRange:
        case rc::GIResourceStatus::eBufferSize:
            message = "Averaged reflectance exceeds the GPU buffer limit. Use a lower voxel resolution.";
            break;
        case rc::GIResourceStatus::eOverflow:
            message = "Averaged reflectance exceeds its supported scene size. Use owner reflectance.";
            break;
        default: break;
    }

    return message;
}
} // namespace

void RuntimeDebugUI::EnsureReflectanceBudget()
{
    if (m_draft.averagedReflectance)
    {
        m_draft.reflectanceBudgetBytes =
            std::max(m_draft.reflectanceBudgetBytes, rc::GetVoxelReflectanceRequiredBytes(m_draft.resolution));
    }
}

void RuntimeDebugUI::MarkGIEdit(bool changed)
{
    if (changed)
    {
        m_dirty       = true;

        m_applyFailed = false;
    }
}

void RuntimeDebugUI::MarkSceneEdit(bool changed)
{
    if (changed)
    {
        m_sceneDirty  = true;

        m_applyFailed = false;
    }
}

void RuntimeDebugUI::ReloadSettings()
{
    m_modelRevision = m_sceneControls.GetRuntimeModelState().revision;

    m_draft         = m_device.GetRendererServer()->GetVoxelGISettings();

    m_sceneDraft    = m_sceneControls.GetRuntimeSceneSettings();

    m_sceneBaseline = m_sceneDraft;

    m_dirty         = false;

    m_sceneDirty    = false;

    m_applyFailed   = false;

    m_status        = "Settings apply to this session only.";
}

void RuntimeDebugUI::ApplyPendingSettings(bool manual)
{
    rc::RendererServer& server               = *m_device.GetRendererServer();

    rc::VoxelGIRuntimeSettings next          = m_draft;

    const rc::VoxelGIRuntimeSettings current = server.GetVoxelGISettings();

    const bool resourceChange                = rc::RequiresVoxelGIRebuild(current, next)
                             || current.shadowMapResolution != next.shadowMapResolution
                             || m_sceneDraft.lightCount != m_sceneBaseline.lightCount;

    const bool pending = m_dirty || m_sceneDirty;

    const bool shouldApply =
        pending && (manual || (m_autoApply && !m_applyFailed && (!resourceChange || !ImGui::IsAnyItemActive())));

    if (shouldApply)
    {
        uint64_t reflectanceBytes                 = 0;

        const rc::GIResourceStatus resourceStatus = server.ValidateVoxelGIResources(next, reflectanceBytes);

        const bool validGI                        = !m_dirty || resourceStatus == rc::GIResourceStatus::eSuccess;

        const bool validScene                     = !m_sceneDirty || ValidateRuntimeSceneSettings(m_sceneDraft);

        if (validGI && validScene)
        {
            bool applied = !m_dirty || server.ApplyVoxelGISettings(next);

            if (applied)
            {
                m_draft = server.GetVoxelGISettings();

                m_dirty = false;

                applied = !m_sceneDirty || m_sceneControls.ApplyRuntimeSceneSettings(m_sceneBaseline, m_sceneDraft);
            }

            if (applied)
            {
                ReloadSettings();

                m_status = "Changes applied to this session.";
            }
            else
            {
                m_applyFailed = true;

                m_status      = "Could not apply all changes. Edit a value or use Apply pending to retry.";
            }
        }
        else
        {
            m_status = validGI ? "Invalid scene values. Check light direction, spot angles, and animation target."
                               : ResourceValidationMessage(resourceStatus);
        }
    }
    else if (pending && !m_applyFailed)
    {
        m_status = m_autoApply ? "Pending resource changes: finish editing to apply." : "Unapplied changes. Use Apply pending.";
    }
}

void RuntimeDebugUI::BuildSettings()
{
    MarkGIEdit(
        ImGui::SliderFloat("Indirect intensity", &m_draft.cone.indirectIntensity, 0, 10, "%.2f", ImGuiSliderFlags_AlwaysClamp));

    MarkGIEdit(ImGui::Checkbox("Shadows", &m_draft.cone.shadows));

    if (ImGui::CollapsingHeader("Voxel resources"))
    {
        int resolution = m_draft.resolution == 64 ? 0 : m_draft.resolution == 128 ? 1 : 2;

        if (ImGui::Combo("Voxel resolution", &resolution, "64\0 128\0 256\0"))
        {
            m_draft.resolution = 64u << resolution;

            EnsureReflectanceBudget();

            MarkGIEdit(true);
        }

        int voxelizer = static_cast<int>(m_draft.voxelizer);

        if (ImGui::Combo("Voxelizer", &voxelizer, "Auto\0Compute\0Geometry\0"))
        {
            m_draft.voxelizer = static_cast<platform::VoxelizerMode>(voxelizer);

            MarkGIEdit(true);
        }

        if (ImGui::Checkbox("Averaged reflectance", &m_draft.averagedReflectance))
        {
            EnsureReflectanceBudget();

            MarkGIEdit(true);
        }

        MarkGIEdit(BudgetInput("Reflectance budget (MiB)", m_draft.reflectanceBudgetBytes));

        if (m_draft.averagedReflectance)
        {
            const uint64_t required = rc::GetVoxelReflectanceRequiredBytes(m_draft.resolution);

            ImGui::Text("Minimum extra memory: %llu MiB", static_cast<unsigned long long>(required / (1024 * 1024)));

            if (m_draft.reflectanceBudgetBytes < required)
            {
                ImGui::TextWrapped("Budget is too small; this change will not be applied.");

                if (ImGui::Button("Use minimum reflectance budget"))
                {
                    EnsureReflectanceBudget();

                    MarkGIEdit(true);
                }
            }
        }

        const rc::VoxelizerBase* voxelizerState = m_device.GetRendererServer()->RequestVoxelizer();

        if (voxelizerState != nullptr && voxelizerState->GetGeometryRevision() != 0)
        {
            ImGui::Text("Active reflectance: %s", voxelizerState->UsesAveragedReflectance() ? "averaged" : "owner");
        }

        MarkGIEdit(UIntSlider("Shadow resolution", m_draft.shadowMapResolution, 128, 2048));

        bool async = m_draft.asyncCompute == platform::AsyncComputeMode::eAuto;

        if (ImGui::Checkbox("Async compute", &async))
        {
            m_draft.asyncCompute = async ? platform::AsyncComputeMode::eAuto : platform::AsyncComputeMode::eDisabled;

            MarkGIEdit(true);
        }
    }

    if (ImGui::CollapsingHeader("Cone tracing"))
    {
        int cones = m_draft.cone.coneCount == 4 ? 0 : 1;

        if (ImGui::Combo("Cone count", &cones, "4\0 6\0"))
        {
            m_draft.cone.coneCount = cones == 0 ? 4 : 6;

            MarkGIEdit(true);
        }

        MarkGIEdit(
            ImGui::SliderFloat("Cone angle", &m_draft.cone.coneAngleDegrees, 10, 90, "%.1f", ImGuiSliderFlags_AlwaysClamp));

        MarkGIEdit(ImGui::SliderFloat("Step scale", &m_draft.cone.stepScale, 0.25f, 2, "%.2f", ImGuiSliderFlags_AlwaysClamp));

        MarkGIEdit(ImGui::SliderFloat("Normal bias (voxels)", &m_draft.cone.normalBiasVoxels, 0.5f, 4, "%.2f",
                                      ImGuiSliderFlags_AlwaysClamp));

        MarkGIEdit(ImGui::SliderFloat("Max distance (grid lengths)", &m_draft.cone.maxDistanceGridLengths, 0.01f, 2, "%.3f",
                                      ImGuiSliderFlags_AlwaysClamp));

        MarkGIEdit(UIntSlider("Cone steps", m_draft.cone.maxSteps, 8, 512));
    }

    if (ImGui::CollapsingHeader("GI contributions"))
    {
        MarkGIEdit(ImGui::Checkbox("Analytic light contribution", &m_draft.cone.analyticLighting));

        MarkGIEdit(ImGui::Checkbox("Environment contribution", &m_draft.cone.environmentLighting));

        MarkGIEdit(ImGui::Checkbox("Emissive contribution", &m_draft.cone.emissiveLighting));

        ImGui::TextWrapped("Controls diffuse GI contributions. Direct lighting is unchanged.");
    }
}
} // namespace zen::ui
