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
#include "Graphics/RenderCore/V2/ShaderProgram.h"
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

        ApplyVoxelGIPreset(VoxelGIPreset::Medium, settings);

        settings.resolution = platform::ConfigLoader::GetInstance().GetVoxelResolution();
    }

    m_giSelectionReason = ResolveVoxelGIAutomaticProvider(settings, m_pRenderDevice->GetGPUInfo());
    LOGI("GI preset {}: {}", GetVoxelGIPresetName(GetVoxelGIPreset(settings)), m_giSelectionReason);
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

    // Warm the supported programs registered for it before frames are queued; driver compilation
    // would otherwise stall the later frames that first use them. Unsupported programs are never
    // registered, so this compiles nothing the device cannot run.
    for (NameID name : ShaderProgramManager::GetInstance().GetInitializationWarmups())
    {
        ShaderProgram* program = ShaderProgramManager::GetInstance().RequestShaderProgram(name);

        if (program != nullptr && program->GetShader() != nullptr
            && m_pRenderDevice->GetOrCreateComputePipeline(program->GetShader()) == nullptr)
        {
            LOGW("Pipeline warmup failed for '{}'", name.CStr());
        }
    }
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

RenderFrameResult RendererServer::DispatchRenderWorkloads(const RenderView& requestedView,
                                                          RenderOverlay*    overlay,
                                                          bool              drawScene)
{
    RenderView view = requestedView;

    const bool presentationView =
        m_pPresentationViewport != nullptr && view.color == m_pPresentationViewport->GetColorBackBuffer();

    m_pRenderDevice->PrepareFrame();

    if (presentationView)
    {
        // Recreation can replace the native attachments after the caller captured its view.
        view.color  = m_pPresentationViewport->GetColorBackBuffer();

        view.depth  = m_pPresentationViewport->GetDepthStencilBackBuffer();

        view.width  = m_pPresentationViewport->GetWidth();

        view.height = m_pPresentationViewport->GetHeight();
    }

    RenderFrameResult result = RenderFrameResult::eFailed;

    m_status.requested       = m_renderOption == RenderOption::eVoxelGI ? RenderAlgorithm::eVoxelGI : RenderAlgorithm::ePBR;

    m_frameRenderOption      = m_status.fallbackReason.empty() ? m_renderOption : RenderOption::ePBR;

    const bool renderScene =
        drawScene && m_pScene != nullptr && view.color != nullptr && view.depth != nullptr && view.width > 0 && view.height > 0;

    bool succeeded         = !m_pRenderDevice->AreSubmissionsBlocked() && (!renderScene || m_pScene->Update());

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

        m_frameVisibility         = false;

        m_debugDescription        = {};

        m_debugDescription.output = m_debug.output;

        if (succeeded && renderScene)
        {
            const bool hybridDebug = m_debug.output >= DebugOutput::eGIRawSky && m_debug.output <= DebugOutput::eGIRejection;
            const bool diagnostic =
                m_debug.output != DebugOutput::eFinal && m_debug.output != DebugOutput::eDepth && !hybridDebug;

            if (m_frameRenderOption == RenderOption::eVoxelGI && !m_pVoxelGI->Init())
            {
                m_frameRenderOption     = RenderOption::ePBR;

                m_status.fallbackReason = "Voxelizer or GI resource initialization failed.";
            }

            if (m_frameRenderOption == RenderOption::eVoxelGI)
            {
                const bool visibilityReady = m_pVoxelGI->BuildRayQueryGraph();
                m_frameGI                  = (visibilityReady || m_pVoxelGI->IsPreparingField()) && !diagnostic;
                m_frameVisibility          = true;
                if (!visibilityReady && !m_pVoxelGI->IsPreparingField())
                {
                    m_frameRenderOption     = RenderOption::ePBR;
                    m_status.fallbackReason = "Scene visibility preparation failed.";
                }
            }

            if (diagnostic)
            {
                BuildDiagnosticFrame(view);
            }
            else
            {
                // Shadow maps serve map-shadowed lighting and near-light visibility of field GI hits.
                const bool shadowMaps = !m_pVoxelGI->UsesRayShadows() || m_pVoxelGI->UsesFieldTraversal();
                if (m_frameRenderOption == RenderOption::eVoxelGI && shadowMaps
                    && !m_pSceneShadows->Prepare(*m_pScene, m_pVoxelGI->GetSettings().shadows, false))
                {
                    m_frameRenderOption     = RenderOption::ePBR;

                    m_status.fallbackReason = "Shadow memory preflight or allocation failed.";
                }

                m_pSkyboxRenderer->BuildRenderGraph(view);

                if (m_frameRenderOption == RenderOption::eVoxelGI)
                {
                    // The radiance voxels provide indirect light from the first frame. Ray/probe
                    // gather starts when its visibility is ready, with fresh temporal history.
                    const bool rayGather = m_pVoxelGI->UsesFieldTraversal() || m_pVoxelGI->UsesHardwareQueries();
                    m_pDeferredLightingRenderer->BuildGBufferGraph(view, rayGather);

                    m_pVoxelizer->BuildVoxelizationGraph();

                    if (!shadowMaps)
                    {
                        m_pSceneShadows->Destroy();
                    }
                    else
                    {
                        m_pSceneShadows->BuildRenderGraph(*m_pScene);
                        m_frameShadows = true;
                    }

                    m_pVoxelGI->BuildRenderGraph(m_frameShadows ? m_pSceneShadows : nullptr);

                    m_pDeferredLightingRenderer->BuildCompositionGraph(
                        view, m_frameRenderOption == RenderOption::eVoxelGI ? m_pVoxelGI : nullptr, m_pSceneShadows);

                    if (m_frameRenderOption == RenderOption::eVoxelGI && m_pDeferredLightingRenderer->GetHybridGI() != nullptr
                        && !m_pDeferredLightingRenderer->GetHybridGI()->IsActive())
                    {
                        m_frameRenderOption     = RenderOption::ePBR;
                        m_status.fallbackReason = "Hybrid GI history allocation failed.";
                    }
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
                else if (hybridDebug)
                {
                    HybridGIRenderer* hybrid = m_pDeferredLightingRenderer->GetHybridGI();
                    if (m_frameRenderOption == RenderOption::eVoxelGI && hybrid != nullptr)
                    {
                        m_debugDescription = hybrid->BuildDebugView(view, m_debug);
                    }
                    else
                    {
                        m_debugDescription.available = false;
                        m_debugDescription.reason    = "Select Voxel GI to inspect hybrid lighting.";
                    }
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
            result = m_pPresentationViewport != nullptr ? m_pRenderDevice->ExecuteRenderGraph(m_pPresentationViewport)
                   : m_pRenderDevice->ExecuteRenderGraph(*pFrameRDG) ? RenderFrameResult::eSubmitted
                                                                     : RenderFrameResult::eFailed;
        }
    }

    if (m_pRenderDevice->AreSubmissionsBlocked())
    {
        result = RenderFrameResult::eBlocked;
    }

    if (m_pVoxelizer != nullptr)
    {
        m_pVoxelizer->OnRenderGraphExecuted(result);
    }

    m_pSkyboxRenderer->OnRenderGraphExecuted(result);
    m_pDeferredLightingRenderer->OnRenderGraphExecuted(result);

    // Visibility preparation records work on PBR fallback frames as well.
    if (m_frameVisibility)
    {
        m_pVoxelGI->OnVisibilityGraphExecuted(result);
    }

    if (m_frameGI)
    {
        m_pVoxelGI->OnRenderGraphExecuted(result);
    }

    if (m_frameShadows)
    {
        m_pSceneShadows->OnRenderGraphExecuted(result);
    }

    if (result != RenderFrameResult::eSubmitted)
    {
        m_debugDescription.available = false;

        m_debugDescription.reason =
            result == RenderFrameResult::eDeclined ? "Waiting for swapchain recreation." : "Frame submission failed.";
    }

    m_status.effective = m_frameRenderOption == RenderOption::eVoxelGI ? RenderAlgorithm::eVoxelGI : RenderAlgorithm::ePBR;

    return result;
}

void RendererServer::SetRenderingSelection(RenderAlgorithm algorithm, const DebugSelection& selection)
{
    const RenderOption option = algorithm == RenderAlgorithm::eVoxelGI ? RenderOption::eVoxelGI : RenderOption::ePBR;

    if (m_renderOption != option || m_debug.output != selection.output)
    {
        ResetRenderingFailure();

        m_pSceneShadows->OnRenderGraphExecuted(RenderFrameResult::eFailed);
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
            m_pSceneShadows->BuildRenderGraph(*m_pScene);

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

bool RendererServer::IsPreparingGI() const
{
    return m_renderOption == RenderOption::eVoxelGI && m_pVoxelGI != nullptr && m_pVoxelGI->IsPreparingField();
}

bool RendererServer::IsRefiningGI() const
{
    return m_renderOption == RenderOption::eVoxelGI && m_pVoxelGI != nullptr && m_pVoxelGI->IsRefiningField();
}

void RendererServer::SetRenderScene(RenderScene* pScene)
{
    ResetRenderingFailure();

    m_pScene                  = pScene;

    m_debugDescription        = {};

    m_debugDescription.output = m_debug.output;

    m_pSceneShadows->OnRenderGraphExecuted(RenderFrameResult::eFailed);

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
