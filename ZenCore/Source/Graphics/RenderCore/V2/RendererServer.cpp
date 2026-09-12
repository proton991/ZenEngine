#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/SkyboxRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/DeferredLightingRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/GeometryVoxelizer.h"
#include "Graphics/RenderCore/V2/Renderer/ComputeVoxelizer.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
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
    // Voxel mode displays albedo. Add shadow/radiance producers only with a consumer.
}

void RendererServer::Destroy()
{
    m_pDeferredLightingRenderer->Destroy();
    ZEN_DELETE(m_pDeferredLightingRenderer);

    m_pSkyboxRenderer->Destroy();
    ZEN_DELETE(m_pSkyboxRenderer);

    m_pVoxelizer->Destroy();
    ZEN_DELETE(m_pVoxelizer);
}

void RendererServer::DispatchRenderWorkloads()
{
    m_pScene->Update();

    RenderGraph* pFrameRDG = m_pRenderDevice->GetCurrentFrameRDG();
    VERIFY_EXPR(pFrameRDG != nullptr);
    pFrameRDG->Begin();

    if (m_renderOption == RenderOption::eVoxelize)
    {
        m_pSkyboxRenderer->BuildRenderGraph();
        m_pVoxelizer->BuildRenderGraph();
    }
    else
    {
        m_pSkyboxRenderer->BuildRenderGraph();
        m_pDeferredLightingRenderer->BuildRenderGraph();
    }

    const bool succeeded = pFrameRDG->End() && m_pRenderDevice->ExecuteRenderGraph(m_pViewport);
    m_pSkyboxRenderer->OnRenderGraphExecuted(succeeded);

    if (!succeeded && m_renderOption == RenderOption::eVoxelize)
    {
        m_pVoxelizer->RequestVoxelization();
    }
}

void RendererServer::SetRenderScene(RenderScene* pScene)
{
    m_pScene = pScene;
    m_pSkyboxRenderer->SetRenderScene(pScene);
    m_pDeferredLightingRenderer->SetRenderScene(pScene);
    m_pVoxelizer->SetRenderScene(pScene);
}

} // namespace zen::rc
