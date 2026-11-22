#include "Utils/Errors.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/SceneShadowRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelizerBase.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"

namespace zen::rc
{
bool RendererServer::ValidateShadowResources(const RenderingSettings& settings, uint32_t faces) const
{
    const VoxelGISettings& gi = settings.gi.cone;

    // Scene-specific HW build failures are resolved before the frame's shadow-map Prepare().
    const bool maps = settings.debug.output == DebugOutput::eShadow
                   || (settings.algorithm == RenderAlgorithm::eVoxelGI && gi.shadows
                       && !gi.UsesRayShadows(m_pRenderDevice->GetGPUInfo().rayQuery.IsUsable()));

    return !maps || (m_pSceneShadows != nullptr && m_pSceneShadows->Preflight(settings.gi.shadowMapResolution, faces));
}

VoxelGIRuntimeSettings RendererServer::GetVoxelGISettings() const
{
    VoxelGIRuntimeSettings settings = GetEffectiveVoxelGISettings();

    settings.optimal                = false;

    if (m_giAutomaticTier)
    {
        settings.cone.rayProvider = VoxelGISettings::RayProvider::Auto;
    }
    return settings;
}

VoxelGIRuntimeSettings RendererServer::GetRequestedVoxelGISettings() const
{
    VoxelGIRuntimeSettings settings = m_giRequested.optimal ? m_giRequested : GetVoxelGISettings();

    settings.asyncCompute           = m_pRenderDevice->GetAsyncComputeMode();

    return settings;
}

VoxelGIRuntimeSettings RendererServer::GetEffectiveVoxelGISettings() const
{
    VoxelGIRuntimeSettings settings = m_giSettings;

    settings.asyncCompute           = m_pRenderDevice->GetAsyncComputeMode();

    if (m_pVoxelGI != nullptr)
    {
        settings.cone = m_pVoxelGI->GetSettings();
    }
    return settings;
}

GIResourceStatus RendererServer::ValidateVoxelGIResources(const VoxelGIRuntimeSettings& settings,
                                                          uint64_t&                     reflectanceBytes) const
{
    reflectanceBytes = settings.averagedReflectance ? GetVoxelReflectanceRequiredBytes(settings.resolution) : 0;

    GIResourceStatus status =
        ValidateVoxelGIRuntimeSettings(settings) ? GIResourceStatus::eSuccess : GIResourceStatus::eInvalidInput;

    if (status == GIResourceStatus::eSuccess && settings.averagedReflectance)
    {
        const uint64_t triangles = m_pScene != nullptr ? m_pScene->GetVoxelTriangleCount() : 0;

        status = ValidateVoxelReflectanceResources(settings.resolution, triangles, settings.reflectanceBudgetBytes, 0,
                                                   m_pRenderDevice->GetGPUInfo(), reflectanceBytes);
    }

    return status;
}

void RendererServer::DestroyVoxelGIResources()
{
    if (m_pVoxelGI != nullptr)
    {
        m_pVoxelGI->Destroy();

        ZEN_DELETE(m_pVoxelGI);

        m_pVoxelGI = nullptr;
    }

    if (m_pVoxelizer != nullptr)
    {
        m_pVoxelizer->Destroy();

        ZEN_DELETE(m_pVoxelizer);
    }

    m_pVoxelizer = nullptr;
}

bool RendererServer::ApplyVoxelGISettings(const VoxelGIRuntimeSettings& requested, bool retryFailedResources)
{
    VoxelGIRuntimeSettings settings        = requested;
    const char*            selectionReason = ResolveVoxelGIAutomaticProvider(settings, m_pRenderDevice->GetGPUInfo());
    const bool             automatic =
        IsOptimalVoxelGIConfiguration(requested) || requested.cone.rayProvider == VoxelGISettings::RayProvider::Auto;

    uint64_t reflectanceBytes = 0;

    bool valid = ValidateVoxelGIResources(settings, reflectanceBytes) == GIResourceStatus::eSuccess && m_pVoxelGI != nullptr
              && m_pRenderDevice->CanReconfigureResources();

    if (valid)
    {
        const VoxelGIRuntimeSettings previous = GetEffectiveVoxelGISettings();

        // Allocation failures stay latched between frames; only an explicit retry rebuilds a failed
        // voxelizer, so interactive cone edits never drain the GPU to repeat a failed allocation.
        const bool rebuild =
            RequiresVoxelGIRebuild(previous, settings) || (retryFailedResources && m_pVoxelizer->HasFailedInitialization());

        const bool resizeShadows = previous.shadowMapResolution != settings.shadowMapResolution;

        valid                    = m_pRenderDevice->SetAsyncComputeMode(settings.asyncCompute);

        if (valid && (rebuild || resizeShadows))
        {
            valid = m_pRenderDevice->PrepareForResourceReconfiguration();
        }

        if (valid)
        {
            if (rebuild || resizeShadows || retryFailedResources)
            {
                ResetRenderingFailure();
            }

            if (rebuild)
            {
                DestroyVoxelGIResources();

                // A replacement voxelizer restarts its revision counter. Do not let it
                // collide with a cached shadow generation after pending geometry edits.
                m_pSceneShadows->OnRenderGraphExecuted(RenderFrameResult::eFailed);

                // Free retired allocations before planning/allocating the replacement budget.
                m_pRenderDevice->CollectCompletedResources();
            }

            m_giSettings        = settings;
            m_giRequested       = requested;
            m_giSelectionReason = selectionReason;
            m_giAutomaticTier   = automatic;

            if (rebuild)
            {
                CreateVoxelGIResources();
            }

            m_pVoxelGI->SetSettings(settings.cone);

            m_pVoxelGI->SetAutomaticTier(automatic);

            m_pSceneShadows->SetResolution(settings.shadowMapResolution);

            if (resizeShadows)
            {
                m_pVoxelGI->OnRenderGraphExecuted(RenderFrameResult::eFailed);
            }

            m_pRenderDevice->CollectCompletedResources();

            // GI edits apply every frame while a control is dragged; log resource changes only.
            if (rebuild || resizeShadows)
            {
                LOGI("Applied runtime GI settings: grid={}, resources={}", settings.resolution,
                     rebuild ? "recreated" : "retained");
            }
        }
    }

    return valid;
}
} // namespace zen::rc
