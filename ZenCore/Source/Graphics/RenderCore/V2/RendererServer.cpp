#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelGIRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/DynamicVoxelGIRenderer.h"
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
    DynamicVoxelGISettings settings;
    if (!LoadDynamicVoxelGISettings(platform::ConfigLoader::GetInstance(), settings))
    {
        LOGW("Using legacy cone defaults after rejected dynamic voxel GI settings");
    }
    m_giSelection = ResolveVoxelGISelection(settings);
    m_dynamicSettings = settings;
    LOGI("Selected voxel GI method: cone; query backend: {}; {}", m_giSelection.backend,
         m_giSelection.reason);

    m_pDeferredLightingRenderer = ZEN_NEW() DeferredLightingRenderer(m_pRenderDevice, m_pViewport);
    m_pDeferredLightingRenderer->Init();

    m_pSkyboxRenderer = ZEN_NEW() SkyboxRenderer(m_pRenderDevice, m_pViewport);
    m_pSkyboxRenderer->Init();

    const platform::VoxelizerMode requestedMode =
        platform::ConfigLoader::GetInstance().GetVoxelizerMode();
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
    m_pVoxelizer    = CreateVoxelizer(m_pViewport, GI_ALL);
    m_pVoxelGI      = ZEN_NEW() VoxelGIRenderer(m_pRenderDevice, m_pVoxelizer);
    m_pSceneShadows = ZEN_NEW() SceneShadowRenderer(m_pRenderDevice);
}

void RendererServer::Destroy()
{
    if (m_pDynamicVoxelGI != nullptr)
    {
        m_pDynamicVoxelGI->Destroy();
        ZEN_DELETE(m_pDynamicVoxelGI);
    }
    m_pSceneShadows->Destroy();
    ZEN_DELETE(m_pSceneShadows);
    m_pDeferredLightingRenderer->Destroy();
    ZEN_DELETE(m_pDeferredLightingRenderer);

    m_pSkyboxRenderer->Destroy();
    ZEN_DELETE(m_pSkyboxRenderer);

    m_pVoxelGI->Destroy();
    ZEN_DELETE(m_pVoxelGI);

    for (VoxelizerBase* voxelizer : {m_pVoxelizer, m_pStaticVoxels, m_pDynamicVoxels})
    {
        if (voxelizer != nullptr)
        {
            voxelizer->Destroy();
            ZEN_DELETE(voxelizer);
        }
    }
}

bool RendererServer::DispatchRenderWorkloads()
{
    m_frameRenderOption    = m_renderOption;
    bool succeeded         = m_pScene->Update();
    RenderGraph* pFrameRDG = m_pRenderDevice->GetCurrentFrameRDG();
    VERIFY_EXPR(pFrameRDG != nullptr);
    if (succeeded)
    {
        const bool dynamicGI = m_frameRenderOption == RenderOption::eVoxelGI && PrepareVoxelGI();
        succeeded = pFrameRDG->Begin() && BuildClassVoxelization();
        if (succeeded)
        {
            if (m_frameRenderOption == RenderOption::eVoxelize && !m_pVoxelizer->EnsureReady())
            {
                LOGE("Voxel visualization unavailable; selecting PBR");
                m_frameRenderOption = RenderOption::ePBR;
            }
            if (m_frameRenderOption == RenderOption::eVoxelGI &&
                (!m_pVoxelGI->Init() ||
                 !m_pSceneShadows->Prepare(*m_pScene, m_pVoxelGI->GetSettings().shadows,
                                           dynamicGI)))
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
                m_pDeferredLightingRenderer->BuildGBufferGraph(dynamicGI);
                m_pVoxelizer->BuildVoxelizationGraph();
                m_pSceneShadows->BuildRenderGraph(*m_pScene,
                                                  m_pVoxelizer->GetRecordedGeometryRevision());
                m_pVoxelGI->BuildRenderGraph();
                if (dynamicGI)
                {
                    StaticVoxelGIInputs inputs{
                        m_classVisibility.GetInfo().grid,
                        m_pStaticVoxels->GetRecordedVisibilityRevision(),
                        m_pStaticVoxels->GetOccupiedList(),
                        m_pStaticVoxels->GetGridToList(),
                        m_pStaticVoxels->GetOccupiedCount(),
                        m_pDynamicVoxels->GetOccupiedCount(),
                        m_pStaticVoxels->GetVoxelTextures().pOwner,
                        m_pDynamicVoxels->GetVoxelTextures().pOwner,
                        m_pDynamicVoxels->GetGridToList(),
                        glm::uvec2(m_pViewport->GetWidth(), m_pViewport->GetHeight())};
                    inputs.historyRevision = m_pScene->GetGIHistoryRevision();
                    inputs.scene               = m_pScene;
                    inputs.shadowMaps          = m_pSceneShadows;
                    inputs.normal             = m_pStaticVoxels->GetVoxelTextures().pNormal;
                    inputs.dynamicNormal      = m_pDynamicVoxels->GetVoxelTextures().pNormal;
                    inputs.environment        = m_pScene->GetEnvTexture().pSkybox->GetDefaultView();
                    inputs.environmentSampler = m_pScene->GetEnvTexture().pPrefilteredSampler;
                    inputs.environmentRevision = m_pScene->GetEnvironmentRevision();
                    const SceneUniformData& lighting =
                        *reinterpret_cast<const SceneUniformData*>(m_pScene->GetSceneUniformData());
                    succeeded = m_pDynamicVoxelGI->BuildRenderGraph(
                        inputs, m_classVisibility, lighting,
                        m_pVoxelGI->GetSettings().indirectIntensity,
                        m_pVoxelGI->GetSettings().shadows);
                }
                m_pDeferredLightingRenderer->BuildCompositionGraph(
                    m_pVoxelGI, m_pSceneShadows, dynamicGI ? m_pDynamicVoxelGI : nullptr);
            }
            else
            {
                m_pDeferredLightingRenderer->BuildRenderGraph();
            }
        }
        succeeded =
            succeeded && pFrameRDG->End() && m_pRenderDevice->ExecuteRenderGraph(m_pViewport);
    }
    for (VoxelizerBase* voxelizer : {m_pVoxelizer, m_pStaticVoxels, m_pDynamicVoxels})
    {
        if (voxelizer != nullptr)
        {
            voxelizer->OnRenderGraphExecuted(succeeded);
        }
    }
    m_pSkyboxRenderer->OnRenderGraphExecuted(succeeded);
    if (m_pDynamicVoxelGI != nullptr)
    {
        m_pDynamicVoxelGI->OnRenderGraphExecuted(succeeded);
    }
    if (m_frameRenderOption == RenderOption::eVoxelGI)
    {
        m_pVoxelGI->OnRenderGraphExecuted(succeeded);
        m_pSceneShadows->OnRenderGraphExecuted(succeeded);
    }
    if (!succeeded)
    {
        m_classVisibility = VoxelDDAProvider();
    }
    return succeeded;
}

void RendererServer::SetRenderScene(RenderScene* pScene)
{
    m_pScene = pScene;
    m_pSceneShadows->OnRenderGraphExecuted(false);
    m_pSkyboxRenderer->SetRenderScene(pScene);
    m_pDeferredLightingRenderer->SetRenderScene(pScene);
    for (VoxelizerBase* voxelizer : {m_pVoxelizer, m_pStaticVoxels, m_pDynamicVoxels})
    {
        if (voxelizer != nullptr)
        {
            voxelizer->SetRenderScene(pScene);
        }
    }
    m_classVisibility = VoxelDDAProvider();
    if (m_pDynamicVoxelGI != nullptr)
    {
        m_pDynamicVoxelGI->OnRenderGraphExecuted(false);
    }
    m_pVoxelGI->SetRenderScene(pScene);
}

bool RendererServer::SetVoxelGIMethod(VoxelGIMethod method)
{
    const bool valid = method == VoxelGIMethod::eAuto || method == VoxelGIMethod::eCone ||
        method == VoxelGIMethod::eDynamicVoxel;
    if (valid && m_dynamicSettings.method != method)
    {
        m_dynamicSettings.method = method;
        m_giSelection            = ResolveVoxelGISelection(m_dynamicSettings);
        if (m_pScene != nullptr)
        {
            m_pScene->InvalidateGIHistory();
        }
    }
    return valid;
}

bool RendererServer::PrepareVoxelGI()
{
    bool enabled = false;
    VoxelGISelection selected = ResolveVoxelGISelection(m_dynamicSettings);
    const bool averaged       = platform::ConfigLoader::GetInstance().GetString(
                                    "voxel_reflectance_policy", "owner") == "averaged";
    const RHIGPUInfo& gpu     = m_pRenderDevice->GetGPUInfo();
    const bool complete = m_pScene->GetVoxelCoverageMask(m_pVoxelizer->GetVoxelBounds()) == GI_ALL;
    if (!complete)
    {
        selected = {
            VoxelGIMethod::eNone, "none",
            "incomplete voxel coverage; using PBR until geometry returns or bounds are explicitly rebuilt"};
    }
    else
    {
        if (m_dynamicSettings.method == VoxelGIMethod::eDynamicVoxel &&
            m_dynamicSettings.backend != VoxelGIQueryBackend::eHardwareRT)
        {
            selected.reason =
                "Dynamic voxel GI requires seven G-buffer color attachments; using cone";
            if (gpu.maxColorAttachments >= 7)
            {
                const uint64_t extent = RenderConfig::GetInstance().offScreenFbSize;
                const uint64_t surfaces =
                    extent * extent * 16 * GRenderFrameState.GetNumFramesInFlight();
                uint32_t capacity = 0;
                uint64_t peak     = 0;
                enabled =
                    PlanStaticVoxelGIResources(
                        m_dynamicSettings.resolution, averaged, m_dynamicSettings.memoryBudgetBytes,
                        gpu, capacity, peak, surfaces, 0, m_dynamicSettings.compactCache,
                        m_dynamicSettings.raysPerFace) == GIResourceStatus::eSuccess;
                selected.reason = "Dynamic voxel GI memory preflight rejected; using cone";
                if (enabled)
                {
                    enabled = EnableClassVoxelization(m_dynamicSettings.memoryBudgetBytes);
                    if (enabled && m_pDynamicVoxelGI == nullptr)
                    {
                        m_pDynamicVoxelGI = ZEN_NEW() DynamicVoxelGIRenderer(m_pRenderDevice);
                    }
                    enabled =
                        enabled && m_pDynamicVoxelGI->Init(m_dynamicSettings, averaged, surfaces);
                }
                if (enabled)
                {
                    selected = {
                        VoxelGIMethod::eDynamicVoxel, "voxel_dda",
                        "directional GI; cache initialization/overflow/unknown uses cone for the frame"};
                }
            }
        }
        const bool budgeted = m_dynamicSettings.memoryBudgetBytes != 0 ||
            m_dynamicSettings.method == VoxelGIMethod::eDynamicVoxel;
        if (!enabled && budgeted)
        {
            uint64_t peak = 0;
            GIResourceStatus status;
            if (m_pDynamicVoxelGI != nullptr && m_pDynamicVoxelGI->GetResourceBytes() != 0)
            {
                // Its checked reserve already includes retained classes, cone and attachments.
                peak   = m_pDynamicVoxelGI->GetResourceBytes();
                status = peak <= m_dynamicSettings.memoryBudgetBytes ? GIResourceStatus::eSuccess :
                                                                       GIResourceStatus::eBudget;
            }
            else if (m_pStaticVoxels != nullptr || m_pDynamicVoxels != nullptr)
            {
                status =
                    ValidateVoxelClassResources(m_dynamicSettings.resolution, averaged,
                                                m_dynamicSettings.memoryBudgetBytes, 0, gpu, peak);
            }
            else
            {
                status =
                    ValidateVoxelConeResources(m_dynamicSettings.resolution, averaged,
                                               m_dynamicSettings.memoryBudgetBytes, 0, gpu, peak);
            }
            if (status != GIResourceStatus::eSuccess)
            {
                selected = {VoxelGIMethod::eNone, "none",
                            "cone fallback exceeds the GI budget; using PBR"};
            }
        }
    }
    if (selected.method == VoxelGIMethod::eNone)
    {
        m_frameRenderOption = RenderOption::ePBR;
    }
    if (selected.method != m_giSelection.method || selected.reason != m_giSelection.reason)
    {
        LOGI("Selected voxel GI method: {}; query backend: {}; {}",
             selected.method == VoxelGIMethod::eNone ? "pbr" :
                 enabled                             ? "dynamic_voxel" :
                                                       "cone",
             selected.backend, selected.reason);
    }
    m_giSelection = selected;
    return enabled;
}

VoxelizerBase* RendererServer::CreateVoxelizer(RHIViewport* viewport, uint32_t classMask)
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
    if (voxelizer->ConfigureClass(classMask))
    {
        voxelizer->Init();
    }
    return voxelizer;
}

bool RendererServer::EnableClassVoxelization(uint64_t budgetBytes)
{
    const bool averaged = platform::ConfigLoader::GetInstance().GetString(
                              "voxel_reflectance_policy", "owner") == "averaged";
    uint64_t peakBytes  = 0;
    const GIResourceStatus status =
        ValidateVoxelClassResources(m_pVoxelizer->GetVoxelTexResolution(), averaged, budgetBytes, 0,
                                    m_pRenderDevice->GetGPUInfo(), peakBytes);
    bool valid = m_pScene != nullptr && status == GIResourceStatus::eSuccess;
    if (valid && m_pStaticVoxels == nullptr && m_pDynamicVoxels == nullptr)
    {
        VoxelizerBase* staticVoxels  = CreateVoxelizer(nullptr, GI_STATIC);
        VoxelizerBase* dynamicVoxels = CreateVoxelizer(nullptr, GI_DYNAMIC);
        for (VoxelizerBase* voxelizer : {staticVoxels, dynamicVoxels})
        {
            voxelizer->SetRenderScene(m_pScene);
            valid = voxelizer->EnableRadianceInputs() && voxelizer->EnableCompaction() && valid;
        }
        if (valid)
        {
            m_pStaticVoxels  = staticVoxels;
            m_pDynamicVoxels = dynamicVoxels;
        }
        else
        {
            for (VoxelizerBase* voxelizer : {staticVoxels, dynamicVoxels})
            {
                voxelizer->Destroy();
                ZEN_DELETE(voxelizer);
            }
        }
    }
    if (!valid)
    {
        LOGW("Class voxelization rejected: status {}, transition peak {} bytes",
             static_cast<uint32_t>(status), peakBytes);
    }
    return valid;
}

bool RendererServer::BuildClassVoxelization()
{
    bool valid = true;
    if (m_pStaticVoxels != nullptr && m_pDynamicVoxels != nullptr)
    {
        m_pStaticVoxels->BuildVoxelizationGraph();
        m_pDynamicVoxels->BuildVoxelizationGraph();
        const GIGridUniform grid{
            Vec4(m_pStaticVoxels->GetSceneMinPoint(), m_pStaticVoxels->GetVoxelSize()),
            glm::uvec4(m_pStaticVoxels->GetVoxelTexResolution(),
                       m_pScene->GetVoxelCoverageMask(m_pStaticVoxels->GetVoxelBounds()), 0, 0),
            glm::uvec4(m_pStaticVoxels->UsesAveragedReflectance() ? 1 : 0,
                       m_pDynamicVoxels->UsesAveragedReflectance() ? 1 : 0, 0, 0)};
        valid = m_classVisibility.Prepare(
            m_pStaticVoxels->GetVoxelTextures(), m_pDynamicVoxels->GetVoxelTextures(), grid,
            m_pScene->GetGeometryRevision() + m_pScene->GetSurfaceRevision());
    }
    return valid;
}
} // namespace zen::rc
