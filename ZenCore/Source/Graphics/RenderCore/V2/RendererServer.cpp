#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/SkyboxRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/DeferredLightingRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/ShadowMapRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/GeometryVoxelizer.h"
#include "Graphics/RenderCore/V2/Renderer/ComputeVoxelizer.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelGIRenderer.h"
#include "Graphics/RenderCore/V2/RenderScene.h"

namespace zen::rc
{
RendererServer::RendererServer(RenderDevice* pRenderDevice, RHIViewport* pViewport) :
    m_pRenderDevice(pRenderDevice), m_pViewport(pViewport)
{}

void RendererServer::Init()
{
    m_pDeferredLightingRenderer = ZEN_NEW() DeferredLightingRenderer(m_pRenderDevice, m_pViewport);
    m_pDeferredLightingRenderer->Init();

    m_pSkyboxRenderer = ZEN_NEW() SkyboxRenderer(m_pRenderDevice, m_pViewport);
    m_pSkyboxRenderer->Init();

    if (m_pRenderDevice->GetGPUInfo().supportGeometryShader)
    {
        m_pVoxelizer = ZEN_NEW() GeometryVoxelizer(m_pRenderDevice, m_pViewport);
    }
    else
    {
        m_pVoxelizer = ZEN_NEW() ComputeVoxelizer(m_pRenderDevice, m_pViewport);
    }
    m_pVoxelizer->Init();

    m_pShadowMapRenderer = ZEN_NEW() ShadowMapRenderer(m_pRenderDevice, m_pViewport);
    m_pShadowMapRenderer->Init();

    m_pVoxelGIRenderer = ZEN_NEW() VoxelGIRenderer(m_pRenderDevice, m_pViewport);
    m_pVoxelGIRenderer->Init();
}

void RendererServer::Destroy()
{
    m_pDeferredLightingRenderer->Destroy();
    ZEN_DELETE(m_pDeferredLightingRenderer);

    m_pShadowMapRenderer->Destroy();
    ZEN_DELETE(m_pShadowMapRenderer);

    m_pSkyboxRenderer->Destroy();
    ZEN_DELETE(m_pSkyboxRenderer);

    m_pVoxelGIRenderer->Destroy();
    ZEN_DELETE(m_pVoxelGIRenderer);

    m_pVoxelizer->Destroy();
    ZEN_DELETE(m_pVoxelizer);
}

void RendererServer::DispatchRenderWorkloads()
{
    m_pScene->Update();

    m_pSkyboxRenderer->PrepareRenderWorkload();

    // m_pShadowMapRenderer->PrepareRenderWorkload();

    // m_pDeferredLightingRenderer->PrepareRenderWorkload();

    std::vector<RenderGraph*> RDGs;
    if (m_renderOption == RenderOption::eVoxelize)
    {
        // m_voxelRenderer->PrepareRenderWorkload();
        m_pVoxelizer->PrepareRenderWorkload();
        m_pShadowMapRenderer->PrepareRenderWorkload();
        m_pVoxelGIRenderer->PrepareRenderWorkload();
        RDGs.push_back(m_pSkyboxRenderer->GetRenderGraph());
        RDGs.push_back(m_pShadowMapRenderer->GetRenderGraph()); // shadowMap
        // RDGs.push_back(m_voxelRenderer->GetRenderGraph());     // voxel
        RDGs.push_back(m_pVoxelizer->GetRenderGraph());       // voxelization
        RDGs.push_back(m_pVoxelGIRenderer->GetRenderGraph()); // voxel GI
    }
    else
    {
        m_pSkyboxRenderer->PrepareRenderWorkload();
        m_pDeferredLightingRenderer->PrepareRenderWorkload();

        RDGs.push_back(m_pSkyboxRenderer->GetRenderGraph());           // skybox
        RDGs.push_back(m_pDeferredLightingRenderer->GetRenderGraph()); // deferred pbr
    }

    m_pRenderDevice->ExecuteFrame(m_pViewport, RDGs);
}

void RendererServer::SetRenderScene(RenderScene* pScene)
{
    m_pScene = pScene;
    m_pSkyboxRenderer->SetRenderScene(pScene);
    m_pDeferredLightingRenderer->SetRenderScene(pScene);
    m_pVoxelizer->SetRenderScene(pScene);
    m_pShadowMapRenderer->SetRenderScene(pScene);
    m_pVoxelGIRenderer->SetRenderScene(pScene);
}

void RendererServer::ViewportResizeCallback()
{
    m_pDeferredLightingRenderer->OnResize();
    m_pSkyboxRenderer->OnResize();
    m_pVoxelizer->OnResize();
}
} // namespace zen::rc