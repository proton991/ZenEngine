#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/SkyboxRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/SceneRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelRenderer.h"

namespace zen::rc
{
RendererServer::RendererServer(RenderDevice* renderDevice, rhi::RHIViewport* viewport) :
    m_renderDevice(renderDevice), m_viewport(viewport)
{}

void RendererServer::Init()
{
    m_sceneRenderer = new SceneRenderer(m_renderDevice, m_viewport);
    m_sceneRenderer->Init();

    m_skyboxRenderer = new SkyboxRenderer(m_renderDevice, m_viewport);
    m_skyboxRenderer->Init();

    if (m_renderDevice->SupportVoxelizer())
    {
        m_voxelRenderer = new VoxelRenderer(m_renderDevice, m_viewport);
        m_voxelRenderer->Init();
    }
}

void RendererServer::Destroy()
{
    m_sceneRenderer->Destroy();
    m_skyboxRenderer->Destroy();
    if (m_renderDevice->SupportVoxelizer())
    {
        m_voxelRenderer->Destroy();
    }
}

void RendererServer::SetRenderScene(RenderScene* scene)
{
    m_sceneRenderer->SetRenderScene(scene);
    if (m_renderDevice->SupportVoxelizer())
    {
        m_voxelRenderer->SetRenderScene(scene);
    }
}
} // namespace zen::rc