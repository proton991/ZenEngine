#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/DynamicVoxelGIRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/SceneShadowRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelizerBase.h"
#include "Graphics/RenderCore/V2/RenderScene.h"

namespace zen::rc
{
VoxelGIRuntimeSettings RendererServer::GetVoxelGISettings() const
{
    VoxelGIRuntimeSettings settings = m_giSettings;

    settings.asyncCompute = m_pRenderDevice->GetAsyncComputeMode();

    if (m_pVoxelGI != nullptr)
    {
        settings.cone = m_pVoxelGI->GetSettings();
    }

    if (m_pDynamicVoxelGI != nullptr && m_pDynamicVoxelGI->GetResourceBytes() != 0)
    {
        settings.dynamic = m_pDynamicVoxelGI->GetSettings();

        // Method selection is owned by the server, including while cone is active.
        settings.dynamic.method = m_giSettings.dynamic.method;
    }

    return settings;
}

void RendererServer::DestroyVoxelGIResources()
{
    if (m_pDynamicVoxelGI != nullptr)
    {
        m_pDynamicVoxelGI->Destroy();

        ZEN_DELETE(m_pDynamicVoxelGI);

        m_pDynamicVoxelGI = nullptr;
    }

    if (m_pVoxelGI != nullptr)
    {
        m_pVoxelGI->Destroy();

        ZEN_DELETE(m_pVoxelGI);

        m_pVoxelGI = nullptr;
    }

    for (VoxelizerBase* voxelizer : {m_pVoxelizer, m_pStaticVoxels, m_pDynamicVoxels})
    {
        if (voxelizer != nullptr)
        {
            voxelizer->Destroy();

            ZEN_DELETE(voxelizer);
        }
    }

    m_pVoxelizer = m_pStaticVoxels = m_pDynamicVoxels = nullptr;

    m_classVisibility = VoxelDDAProvider();

    m_giDiagnosticsReadback = nullptr;

    m_recordedGIDiagnostics = false;
}

bool RendererServer::ApplyVoxelGISettings(const VoxelGIRuntimeSettings& settings)
{
    bool valid = ValidateVoxelGIRuntimeSettings(settings) && m_pVoxelGI != nullptr &&
        m_pRenderDevice->CanReconfigureResources();

    if (valid)
    {
        const VoxelGIRuntimeSettings previous = GetVoxelGISettings();

        const bool rebuild = RequiresVoxelGIRebuild(previous, settings);

        const bool resizeShadows = previous.shadowMapResolution != settings.shadowMapResolution;

        valid = m_pRenderDevice->SetAsyncComputeMode(settings.asyncCompute);

        if (valid && (rebuild || resizeShadows))
        {
            valid = m_pRenderDevice->PrepareForResourceReconfiguration();
        }

        if (valid)
        {
            if (rebuild)
            {
                DestroyVoxelGIResources();

                // A replacement voxelizer restarts its revision counter. Do not let it
                // collide with a cached shadow generation after pending geometry edits.
                m_pSceneShadows->OnRenderGraphExecuted(false);

                // Free retired allocations before planning/allocating the replacement budget.
                m_pRenderDevice->CollectCompletedResources();
            }

            m_giSettings = settings;

            m_voxelizerMode =
                ResolveVoxelizerMode(settings.voxelizer, m_pRenderDevice->GetGPUInfo());

            if (rebuild)
            {
                m_pVoxelizer = CreateVoxelizer(m_pViewport, GI_ALL);

                m_pVoxelizer->SetRenderScene(m_pScene);

                m_pVoxelGI = ZEN_NEW() VoxelGIRenderer(m_pRenderDevice, m_pVoxelizer);

                m_pVoxelGI->SetRenderScene(m_pScene);
            }

            m_pVoxelGI->SetSettings(settings.cone);

            m_pSceneShadows->SetResolution(settings.shadowMapResolution);

            m_pRenderDevice->CollectCompletedResources();

            if (m_pDynamicVoxelGI != nullptr)
            {
                m_pDynamicVoxelGI->SetFiltering(settings.dynamic.temporal,
                                                settings.dynamic.spatialFilter);

                m_pDynamicVoxelGI->SetLighting(settings.dynamic.analyticLighting,
                                               settings.dynamic.environmentLighting,
                                               settings.dynamic.emissiveLighting);

                m_pDynamicVoxelGI->SetTemporalParameters(settings.dynamic.temporalAlpha,
                                                         settings.dynamic.historyGapSeconds,
                                                         settings.dynamic.temporalReferenceHz);

                m_pDynamicVoxelGI->SetCacheBatchSize(settings.dynamic.cacheBatchSize);
            }

            if (m_pScene != nullptr &&
                (rebuild || previous.dynamic.method != settings.dynamic.method))
            {
                m_pScene->InvalidateGIHistory();
            }

            if (rebuild || previous.dynamic.method != settings.dynamic.method)
            {
                m_giSelection = ResolveVoxelGISelection(settings.dynamic);
            }

            LOGI("Applied runtime GI settings: grid={}, voxelizer={}, resources={}",
                 settings.dynamic.resolution,
                 m_voxelizerMode == platform::VoxelizerMode::eGeometry ? "geom" : "comp",
                 rebuild ? "recreated" : "retained");
        }
    }

    return valid;
}
} // namespace zen::rc
