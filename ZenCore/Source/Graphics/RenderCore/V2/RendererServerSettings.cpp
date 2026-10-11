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

    const bool hardwareProvider =
        gi.rayProvider == VoxelGISettings::RayProvider::Auto || gi.rayProvider == VoxelGISettings::RayProvider::Hardware;

    const uint64_t budget = uint64_t(gi.accelerationStructureBudgetMB) * 1024 * 1024;

    // Readiness matters: supported devices can still fall back after a scene/build failure.
    // Recheck the proposed provider and budget so edits which select maps are preflighted.
    const bool hardwareShadows = gi.rtShadows && hardwareProvider && m_pVoxelGI != nullptr && m_pVoxelGI->UsesHardwareQueries()
                              && m_pVoxelGI->GetSceneRayQuery().IsReady()
                              && (budget == 0 || m_pVoxelGI->GetSceneRayQuery().GetMemoryBytes() <= budget);

    const bool maps            = settings.debug.output == DebugOutput::eShadow
                              || (settings.algorithm == RenderAlgorithm::eVoxelGI && gi.shadows && !hardwareShadows);

    return !maps || (m_pSceneShadows != nullptr && m_pSceneShadows->Preflight(settings.gi.shadowMapResolution, faces));
}

VoxelGIRuntimeSettings RendererServer::GetVoxelGISettings() const
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
    VoxelGIRuntimeSettings settings = requested;

    uint64_t reflectanceBytes       = 0;

    bool valid = ValidateVoxelGIResources(settings, reflectanceBytes) == GIResourceStatus::eSuccess && m_pVoxelGI != nullptr
              && m_pRenderDevice->CanReconfigureResources();

    if (valid)
    {
        const VoxelGIRuntimeSettings previous = GetVoxelGISettings();

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
                m_pSceneShadows->OnRenderGraphExecuted(false);

                // Free retired allocations before planning/allocating the replacement budget.
                m_pRenderDevice->CollectCompletedResources();
            }

            m_giSettings    = settings;

            m_voxelizerMode = ResolveVoxelizerMode(settings.voxelizer, m_pRenderDevice->GetGPUInfo());

            if (rebuild)
            {
                m_pVoxelizer = CreateVoxelizer();

                m_pVoxelizer->SetRenderScene(m_pScene);

                m_pVoxelGI = ZEN_NEW() VoxelGIRenderer(m_pRenderDevice, m_pVoxelizer);

                m_pVoxelGI->SetRenderScene(m_pScene);
            }

            m_pVoxelGI->SetSettings(settings.cone);

            m_pSceneShadows->SetResolution(settings.shadowMapResolution);

            if (resizeShadows)
            {
                m_pVoxelGI->OnRenderGraphExecuted(false);
            }

            m_pRenderDevice->CollectCompletedResources();

            // Cone edits apply every frame while a control is dragged; log resource changes only.
            if (rebuild || resizeShadows)
            {
                LOGI("Applied runtime GI settings: grid={}, voxelizer={}, resources={}", settings.resolution,
                     m_voxelizerMode == platform::VoxelizerMode::eGeometry ? "geom" : "comp",
                     rebuild ? "recreated" : "retained");
            }
        }
    }

    return valid;
}
} // namespace zen::rc
