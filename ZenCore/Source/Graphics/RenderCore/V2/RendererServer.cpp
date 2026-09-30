#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/RenderOverlay.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelGIRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/SceneShadowRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/SkyboxRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/DeferredLightingRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/GeometryVoxelizer.h"
#include "Graphics/RenderCore/V2/Renderer/ComputeVoxelizer.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"

namespace zen::rc
{
RendererServer::RendererServer(RenderDevice* pRenderDevice, RHIViewport* pViewport) :
    m_pRenderDevice(pRenderDevice), m_pViewport(pViewport)
{}

void RendererServer::Init()
{
    VoxelGIRuntimeSettings settings;

    if (!LoadVoxelGIRuntimeSettings(platform::ConfigLoader::GetInstance(), settings))
    {
        LOGW("Using GI defaults after rejected configuration");

        settings.resolution = platform::ConfigLoader::GetInstance().GetVoxelResolution();
    }

    m_giSettings = settings;

    m_giSettings.asyncCompute = m_pRenderDevice->GetAsyncComputeMode();

    m_pDeferredLightingRenderer = ZEN_NEW() DeferredLightingRenderer(m_pRenderDevice, m_pViewport);

    m_pDeferredLightingRenderer->Init();

    m_pSkyboxRenderer = ZEN_NEW() SkyboxRenderer(m_pRenderDevice, m_pViewport);

    m_pSkyboxRenderer->Init();

    const platform::VoxelizerMode requestedMode = m_giSettings.voxelizer;

    const platform::VoxelizerMode selectedMode =
        ResolveVoxelizerMode(requestedMode, m_pRenderDevice->GetGPUInfo());

    if (requestedMode == platform::VoxelizerMode::eGeometry &&
        selectedMode != platform::VoxelizerMode::eGeometry)
    {
        LOGW("Geometry voxelization is not supported by the GPU; using comp.");
    }

    LOGI("Selected voxelizer: {}",
         selectedMode == platform::VoxelizerMode::eGeometry ? "geom" : "comp");

    m_voxelizerMode = selectedMode;

    m_pVoxelizer = CreateVoxelizer(m_pViewport);

    m_pVoxelGI = ZEN_NEW() VoxelGIRenderer(m_pRenderDevice, m_pVoxelizer);

    m_pVoxelGI->SetSettings(m_giSettings.cone);

    m_pSceneShadows = ZEN_NEW() SceneShadowRenderer(m_pRenderDevice);

    m_pSceneShadows->SetResolution(m_giSettings.shadowMapResolution);
}

void RendererServer::Destroy()
{
    DestroyVoxelGIResources();

    m_pSceneShadows->Destroy();

    ZEN_DELETE(m_pSceneShadows);

    m_pDeferredLightingRenderer->Destroy();

    ZEN_DELETE(m_pDeferredLightingRenderer);

    m_pSkyboxRenderer->Destroy();

    ZEN_DELETE(m_pSkyboxRenderer);
}

bool RendererServer::DispatchRenderWorkloads(RenderOverlay* overlay)
{
    m_frameRenderOption = m_renderOption;

    bool succeeded = m_pScene->Update();

    RenderGraph* pFrameRDG = m_pRenderDevice->GetCurrentFrameRDG();

    VERIFY_EXPR(pFrameRDG != nullptr);

    if (succeeded)
    {
        if (m_frameRenderOption == RenderOption::eVoxelGI &&
            m_pScene->GetVoxelCoverageMask(m_pVoxelizer->GetVoxelBounds()) != GI_ALL)
        {
            m_frameRenderOption = RenderOption::ePBR;
        }

        succeeded = pFrameRDG->Begin();

        if (succeeded)
        {
            if (m_frameRenderOption == RenderOption::eVoxelize && !m_pVoxelizer->EnsureReady())
            {
                LOGE("Voxel visualization unavailable; selecting PBR");

                m_frameRenderOption = RenderOption::ePBR;
            }

            if (m_frameRenderOption == RenderOption::eVoxelGI &&
                (!m_pVoxelGI->Init() ||
                 !m_pSceneShadows->Prepare(*m_pScene, m_pVoxelGI->GetSettings().shadows, false)))
            {
                LOGE("Voxel GI unavailable; selecting PBR");

                m_frameRenderOption = RenderOption::ePBR;
            }

            m_pSkyboxRenderer->BuildRenderGraph();

            if (m_frameRenderOption == RenderOption::eVoxelize)
            {
                m_pVoxelizer->BuildRenderGraph();
            }
            else if (m_frameRenderOption == RenderOption::eVoxelGI)
            {
                m_pDeferredLightingRenderer->BuildGBufferGraph();

                m_pVoxelizer->BuildVoxelizationGraph();

                m_pSceneShadows->BuildRenderGraph(*m_pScene,
                                                  m_pVoxelizer->GetRecordedGeometryRevision());

                m_pVoxelGI->BuildRenderGraph(m_pSceneShadows);

                m_pDeferredLightingRenderer->BuildCompositionGraph(m_pVoxelGI, m_pSceneShadows);
            }
            else
            {
                m_pDeferredLightingRenderer->BuildRenderGraph();
            }
        }

        if (succeeded && overlay != nullptr)
        {
            succeeded = overlay->BuildRenderGraph(*pFrameRDG, *m_pViewport);
        }

        succeeded =
            succeeded && pFrameRDG->End() && m_pRenderDevice->ExecuteRenderGraph(m_pViewport);
    }

    if (m_pVoxelizer != nullptr)
    {
        m_pVoxelizer->OnRenderGraphExecuted(succeeded);
    }

    m_pSkyboxRenderer->OnRenderGraphExecuted(succeeded);

    if (m_frameRenderOption == RenderOption::eVoxelGI)
    {
        m_pVoxelGI->OnRenderGraphExecuted(succeeded);

        m_pSceneShadows->OnRenderGraphExecuted(succeeded);
    }

    return succeeded;
}

void RendererServer::SetRenderScene(RenderScene* pScene)
{
    m_pScene = pScene;

    m_pSceneShadows->OnRenderGraphExecuted(false);

    m_pSkyboxRenderer->SetRenderScene(pScene);

    m_pDeferredLightingRenderer->SetRenderScene(pScene);

    if (m_pVoxelizer != nullptr)
    {
        m_pVoxelizer->SetRenderScene(pScene);
    }

    m_pVoxelGI->SetRenderScene(pScene);
}

VoxelizerBase* RendererServer::CreateVoxelizer(RHIViewport* viewport)
{
    VoxelizerBase* voxelizer = nullptr;

    if (m_voxelizerMode == platform::VoxelizerMode::eGeometry)
    {
        voxelizer = ZEN_NEW() GeometryVoxelizer(m_pRenderDevice, viewport);
    }
    else
    {
        voxelizer = ZEN_NEW() ComputeVoxelizer(m_pRenderDevice, viewport);
    }

    voxelizer->Init();

    voxelizer->Configure(m_giSettings.resolution, m_giSettings.averagedReflectance,
                         m_giSettings.reflectanceBudgetBytes);

    return voxelizer;
}

} // namespace zen::rc
