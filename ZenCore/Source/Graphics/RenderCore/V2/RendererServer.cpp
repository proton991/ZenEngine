#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/RenderOverlay.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelGIRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/SceneShadowRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/SkyboxRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/DeferredLightingRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/HybridGIRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/GeometryVoxelizer.h"
#include "Graphics/RenderCore/V2/Renderer/ComputeVoxelizer.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"

namespace zen::rc
{
RendererServer::RendererServer(RenderDevice* pRenderDevice, RHIViewport* presentationViewport) :
    m_pRenderDevice(pRenderDevice), m_pPresentationViewport(presentationViewport)
{}

void RendererServer::Init()
{
    VoxelGIRuntimeSettings settings;

    if (!LoadVoxelGIRuntimeSettings(platform::ConfigLoader::GetInstance(), settings))
    {
        LOGW("Using GI defaults after rejected configuration");

        settings.resolution = platform::ConfigLoader::GetInstance().GetVoxelResolution();
    }

    m_giSettings                = settings;

    m_giSettings.asyncCompute   = m_pRenderDevice->GetAsyncComputeMode();

    m_pDeferredLightingRenderer = ZEN_NEW() DeferredLightingRenderer(m_pRenderDevice);

    m_pDeferredLightingRenderer->Init();

    m_pSkyboxRenderer = ZEN_NEW() SkyboxRenderer(m_pRenderDevice);

    m_pSkyboxRenderer->Init();

    const platform::VoxelizerMode requestedMode = m_giSettings.voxelizer;

    const platform::VoxelizerMode selectedMode  = ResolveVoxelizerMode(requestedMode, m_pRenderDevice->GetGPUInfo());

    if (requestedMode == platform::VoxelizerMode::eGeometry && selectedMode != platform::VoxelizerMode::eGeometry)
    {
        LOGW("Geometry voxelization is not supported by the GPU; using comp.");
    }

    LOGI("Selected voxelizer: {}", selectedMode == platform::VoxelizerMode::eGeometry ? "geom" : "comp");

    m_voxelizerMode = selectedMode;

    m_pVoxelizer    = CreateVoxelizer();

    m_pVoxelGI      = ZEN_NEW() VoxelGIRenderer(m_pRenderDevice, m_pVoxelizer);

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

bool RendererServer::DispatchRenderWorkloads(const RenderView& view, RenderOverlay* overlay, bool drawScene)
{
    m_status.requested  = m_renderOption == RenderOption::eVoxelGI ? RenderAlgorithm::eVoxelGI : RenderAlgorithm::ePBR;

    m_frameRenderOption = m_status.fallbackReason.empty() ? m_renderOption : RenderOption::ePBR;

    const bool renderScene =
        drawScene && m_pScene != nullptr && view.color != nullptr && view.depth != nullptr && view.width > 0 && view.height > 0;

    bool succeeded         = !renderScene || m_pScene->Update();

    RenderGraph* pFrameRDG = m_pRenderDevice->GetCurrentFrameRDG();

    VERIFY_EXPR(pFrameRDG != nullptr);

    if (succeeded)
    {
        succeeded = pFrameRDG->Begin();

        if (succeeded && m_pPresentationViewport != nullptr
            && (!renderScene || view.color != m_pPresentationViewport->GetColorBackBuffer()))
        {
            pFrameRDG->AddTransferPass("WorkspaceClear")
                .ClearTexture(m_pPresentationViewport->GetColorBackBuffer(), Color(0.055f, 0.06f, 0.075f, 1.0f));
        }

        m_frameShadows            = false;

        m_frameGI                 = false;

        m_debugDescription        = {};

        m_debugDescription.output = m_debug.output;

        if (succeeded && renderScene)
        {
            const bool diagnostic = m_debug.output != DebugOutput::eFinal && m_debug.output != DebugOutput::eDepth;

            if (diagnostic)
            {
                BuildDiagnosticFrame(view);
            }
            else
            {
                if (m_frameRenderOption == RenderOption::eVoxelGI && !m_pVoxelGI->Init())
                {
                    m_frameRenderOption     = RenderOption::ePBR;

                    m_status.fallbackReason = "Voxelizer or GI resource initialization failed.";
                }

                if (m_frameRenderOption == RenderOption::eVoxelGI
                    && !m_pSceneShadows->Prepare(*m_pScene, m_pVoxelGI->GetSettings().shadows, false))
                {
                    m_frameRenderOption     = RenderOption::ePBR;

                    m_status.fallbackReason = "Shadow memory preflight or allocation failed.";
                }

                m_pSkyboxRenderer->BuildRenderGraph(view);

                if (m_frameRenderOption == RenderOption::eVoxelGI)
                {
                    m_pDeferredLightingRenderer->BuildGBufferGraph(view, m_pVoxelGI->GetSettings().rayProvider
                                                                             != VoxelGISettings::RayProvider::Legacy);

                    m_pVoxelizer->BuildVoxelizationGraph();

                    m_pSceneShadows->BuildRenderGraph(*m_pScene, m_pVoxelizer->GetRecordedGeometryRevision());

                    m_pVoxelGI->BuildRenderGraph(m_pSceneShadows);

                    // The hardware visibility provider covers the full raster scene,
                    // including occluders outside the radiance-cache volume.
                    if (!m_pVoxelGI->UsesHardwareQueries()
                        && m_pScene->GetVoxelCoverageMask(m_pVoxelizer->GetVoxelBounds()) != GI_ALL)
                    {
                        m_frameRenderOption     = RenderOption::ePBR;
                        m_status.fallbackReason = "The scene has incomplete voxel coverage.";
                    }
                    m_pDeferredLightingRenderer->BuildCompositionGraph(
                        view, m_frameRenderOption == RenderOption::eVoxelGI ? m_pVoxelGI : nullptr, m_pSceneShadows);

                    if (m_frameRenderOption == RenderOption::eVoxelGI && m_pDeferredLightingRenderer->GetHybridGI() != nullptr
                        && !m_pDeferredLightingRenderer->GetHybridGI()->IsActive())
                    {
                        m_frameRenderOption     = RenderOption::ePBR;
                        m_status.fallbackReason = "Hybrid GI history allocation failed.";
                    }

                    m_frameShadows = m_frameGI = true;
                }
                else
                {
                    m_pDeferredLightingRenderer->BuildRenderGraph(view);
                }

                m_debugDescription.available = true;

                m_debugDescription.reason.clear();

                m_debugDescription.width          = view.width;

                m_debugDescription.height         = view.height;

                m_debugDescription.format         = view.color->GetFormat();

                m_debugDescription.interpretation = "Final SDR color.";

                if (m_debug.output == DebugOutput::eDepth)
                {
                    m_debugDescription = m_pDeferredLightingRenderer->BuildDebugView(view, m_debug);
                }
            }

            if (!m_debugDescription.available)
            {
                pFrameRDG->AddTransferPass("UnavailableDebugOutput").ClearTexture(view.color, Color(0.04f, 0.02f, 0.04f, 1.0f));
            }
        }

        if (succeeded && overlay != nullptr)
        {
            succeeded = m_pPresentationViewport != nullptr && overlay->BuildRenderGraph(*pFrameRDG, *m_pPresentationViewport);
        }

        succeeded = succeeded && pFrameRDG->End();

        if (succeeded)
        {
            succeeded = m_pPresentationViewport != nullptr ? m_pRenderDevice->ExecuteRenderGraph(m_pPresentationViewport)
                                                           : m_pRenderDevice->ExecuteRenderGraph(*pFrameRDG);
        }
    }

    if (m_pVoxelizer != nullptr)
    {
        m_pVoxelizer->OnRenderGraphExecuted(succeeded);
    }

    m_pSkyboxRenderer->OnRenderGraphExecuted(succeeded);
    m_pDeferredLightingRenderer->OnRenderGraphExecuted(succeeded);

    if (m_frameGI)
    {
        m_pVoxelGI->OnRenderGraphExecuted(succeeded);
    }

    if (m_frameShadows)
    {
        m_pSceneShadows->OnRenderGraphExecuted(succeeded);
    }

    if (!succeeded)
    {
        m_debugDescription.available = false;

        m_debugDescription.reason    = "Frame submission failed.";
    }

    m_status.effective = m_frameRenderOption == RenderOption::eVoxelGI ? RenderAlgorithm::eVoxelGI : RenderAlgorithm::ePBR;

    return succeeded;
}

void RendererServer::SetRenderingSelection(RenderAlgorithm algorithm, const DebugSelection& selection)
{
    const RenderOption option = algorithm == RenderAlgorithm::eVoxelGI ? RenderOption::eVoxelGI : RenderOption::ePBR;

    if (m_renderOption != option || m_debug.output != selection.output)
    {
        ResetRenderingFailure();

        m_pSceneShadows->OnRenderGraphExecuted(false);
    }

    m_renderOption            = option;

    m_debug                   = selection;

    m_debugDescription        = {};

    m_debugDescription.output = selection.output;

    m_debugDescription.reason = "Waiting for the next rendered frame.";
}

void RendererServer::BuildDiagnosticFrame(const RenderView& view)
{
    if (m_debug.output == DebugOutput::eAlbedo || m_debug.output == DebugOutput::eNormal)
    {
        m_pDeferredLightingRenderer->BuildGBufferGraph(view);

        m_debugDescription = m_pDeferredLightingRenderer->BuildDebugView(view, m_debug);
    }
    else if (m_debug.output == DebugOutput::eShadow)
    {
        if (m_pSceneShadows->Prepare(*m_pScene, true, false))
        {
            m_pSceneShadows->BuildRenderGraph(*m_pScene, m_pScene->GetGeometryRevision());

            m_frameShadows     = true;

            m_debugDescription = m_pSceneShadows->BuildDebugView(*m_pScene, view, m_debug);
        }
        else
        {
            m_debugDescription.reason = "Shadow memory preflight or allocation failed.";
        }
    }
    else
    {
        if (m_debug.output == DebugOutput::eVoxels)
        {
            m_pSkyboxRenderer->BuildRenderGraph(view);
        }

        m_debugDescription = m_pVoxelizer->BuildDebugView(view, m_debug);
    }
}

void RendererServer::ResetRenderingFailure()
{
    m_status.fallbackReason.clear();
}

void RendererServer::SetRenderScene(RenderScene* pScene)
{
    ResetRenderingFailure();

    m_pScene                  = pScene;

    m_debugDescription        = {};

    m_debugDescription.output = m_debug.output;

    m_pSceneShadows->OnRenderGraphExecuted(false);

    m_pSkyboxRenderer->SetRenderScene(pScene);

    m_pDeferredLightingRenderer->SetRenderScene(pScene);

    if (m_pVoxelizer != nullptr)
    {
        m_pVoxelizer->SetRenderScene(pScene);
    }

    m_pVoxelGI->SetRenderScene(pScene);
}

VoxelizerBase* RendererServer::CreateVoxelizer()
{
    VoxelizerBase* voxelizer = nullptr;

    if (m_voxelizerMode == platform::VoxelizerMode::eGeometry)
    {
        voxelizer = ZEN_NEW() GeometryVoxelizer(m_pRenderDevice);
    }
    else
    {
        voxelizer = ZEN_NEW() ComputeVoxelizer(m_pRenderDevice);
    }

    voxelizer->Init();

    voxelizer->Configure(m_giSettings.resolution, m_giSettings.averagedReflectance, m_giSettings.reflectanceBudgetBytes);

    return voxelizer;
}

} // namespace zen::rc
