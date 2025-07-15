#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/SkyboxRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/DeferredLightingRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/ShadowMapRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/GeometryVoxelizer.h"
#include "Graphics/RenderCore/V2/Renderer/ComputeVoxelizer.h"
#include "Graphics/RenderCore/V2/RenderScene.h"

namespace zen::rc
{
RendererServer::RendererServer(RenderDevice* renderDevice, rhi::RHIViewport* viewport) :
    m_renderDevice(renderDevice), m_viewport(viewport)
{}

void RendererServer::Init()
{
    m_deferredLightingRenderer = new DeferredLightingRenderer(m_renderDevice, m_viewport);
    m_deferredLightingRenderer->Init();

    m_skyboxRenderer = new SkyboxRenderer(m_renderDevice, m_viewport);
    m_skyboxRenderer->Init();

    if (m_renderDevice->GetGPUInfo().supportGeometryShader)
    {
        m_voxelizer = new GeometryVoxelizer(m_renderDevice, m_viewport);
    }
    else
    {
        m_voxelizer = new ComputeVoxelizer(m_renderDevice, m_viewport);
    }
    m_voxelizer->Init();

    m_shadowMapRenderer = new ShadowMapRenderer(m_renderDevice, m_viewport);
    m_shadowMapRenderer->Init();
}

void RendererServer::Destroy()
{
    m_deferredLightingRenderer->Destroy();
    m_skyboxRenderer->Destroy();
    m_voxelizer->Destroy();
}

void RendererServer::DispatchRenderWorkloads()
{
    m_scene->Update();

    m_skyboxRenderer->PrepareRenderWorkload();

    m_shadowMapRenderer->PrepareRenderWorkload();

    m_deferredLightingRenderer->PrepareRenderWorkload();

    std::vector<RenderGraph*> RDGs;
    if (m_renderOption == RenderOption::eVoxelize)
    {
        // m_voxelRenderer->PrepareRenderWorkload();
        m_voxelizer->PrepareRenderWorkload();
        RDGs.push_back(m_shadowMapRenderer->GetRenderGraph()); // shadowMap
        // RDGs.push_back(m_voxelRenderer->GetRenderGraph());     // voxel
        RDGs.push_back(m_voxelizer->GetRenderGraph()); // voxel
        // RDGs.push_back(skyboxRenderer->GetRenderGraph()); // skybox
        // RDGs.push_back(m_rdg.Get());                      // deferred pbr
    }
    else
    {
        RDGs.push_back(m_skyboxRenderer->GetRenderGraph());           // skybox
        RDGs.push_back(m_deferredLightingRenderer->GetRenderGraph()); // deferred pbr
    }

    m_renderDevice->ExecuteFrame(m_viewport, RDGs);
}

void RendererServer::SetRenderScene(RenderScene* scene)
{
    m_scene = scene;
    m_skyboxRenderer->SetRenderScene(scene);
    m_deferredLightingRenderer->SetRenderScene(scene);
    m_voxelizer->SetRenderScene(scene);
    m_shadowMapRenderer->SetRenderScene(scene);
}

void RendererServer::ViewportResizeCallback()
{
    m_deferredLightingRenderer->OnResize();
    m_skyboxRenderer->OnResize();
    m_voxelizer->OnResize();
}
} // namespace zen::rc