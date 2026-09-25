#include "Graphics/RenderCore/V2/Renderer/DeferredLightingRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelGIRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/DynamicVoxelGIRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/SceneShadowRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/RendererUtils.h"
#include "Graphics/RenderCore/V2/Renderer/SkyboxRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"
#include "Graphics/RenderCore/V2/RenderResource.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RenderCore/V2/ComputeDispatch.h"
#include "Graphics/RenderCore/V2/DynamicVoxelGIPlanning.h"
#include "Graphics/Shared/LightingCapture.h"
#include "SceneGraph/Camera.h"
#include <cmath>

namespace zen::rc
{
namespace
{
void RecordGBufferDraws(RDGPassCmdEncoder& encoder,
                        const HeapVector<SceneMeshDraw>& draws,
                        bool dynamicGI)
{
    GBufferSP::PushConstantsData constants{};

    for (SceneMeshDraw const& draw : draws)
    {
        constants.nodeIndex     = draw.nodeIndex;
        constants.materialIndex = draw.materialIndex;
        if (dynamicGI)
        {
            encoder.SetPushConstants(
                glm::uvec3(draw.nodeIndex, draw.materialIndex, draw.objectClass));
        }
        else
        {
            encoder.SetPushConstants(constants);
        }
        encoder.DrawIndexed(draw.indexCount, 1, draw.firstIndex, 0, 0);
    }
}

void RecordLightingCaptureClear(RDGPassCmdEncoder& encoder,
                                const HeapVector<ComputeDispatchChunk>& chunks)
{
    for (const ComputeDispatchChunk& chunk : chunks)
    {
        encoder.SetPushConstants(glm::uvec2(chunk.firstItem, chunk.itemCount));
        encoder.Dispatch(chunk.groups.x, chunk.groups.y, chunk.groups.z);
    }
}
} // namespace

DeferredLightingRenderer::DeferredLightingRenderer(RenderDevice* pRenderDevice,
                                                   RHIViewport* pViewport) :
    m_pRenderDevice(pRenderDevice), m_pViewport(pViewport)
{}

void DeferredLightingRenderer::Init()
{
    PrepareSamplers();
    const platform::ConfigLoader& config = platform::ConfigLoader::GetInstance();
    bool markersEnabled                  = false;
    float markerSize                     = 0.02f;
    const bool valid = config.ReadBool("light_markers.enabled", markersEnabled) &&
        config.ReadNumber("light_markers.size", markerSize) && std::isfinite(markerSize) &&
        markerSize > 0.0f;
    m_lightMarkerSize = valid && markersEnabled ? markerSize : 0.0f;
    if (!valid)
    {
        LOGW("Invalid light_markers configuration; markers disabled");
    }
}

void DeferredLightingRenderer::Destroy() {}

void DeferredLightingRenderer::PrepareSamplers()
{
    {
        RHISamplerCreateInfo samplerInfo{};
        samplerInfo.borderColor = RHISamplerBorderColor::eFloatOpaqueWhite;
        m_pDepthSampler         = m_pRenderDevice->CreateSampler(samplerInfo);
    }

    const RHISamplerCreateInfo colorSamplerInfo = RHISamplerCreateInfo::CreateLinearRepeat();
    m_pColorSampler                             = m_pRenderDevice->CreateSampler(colorSamplerInfo);
}

bool DeferredLightingRenderer::BuildLightingCaptureClear()
{
    const uint64_t pixels = uint64_t(m_pViewport->GetWidth()) * m_pViewport->GetHeight();
    uint64_t bytes        = 0;
    const RHIGPUInfo& gpu = m_pRenderDevice->GetGPUInfo();
    bool valid            = pixels != 0 && gpu.supportFragmentStoresAndAtomics &&
        ValidateGIStorageBuffer(pixels, ZEN_LIGHTING_CAPTURE_BYTES_PER_PIXEL, gpu, bytes) ==
            GIResourceStatus::eSuccess &&
        m_captureOutput != nullptr && m_captureReadback != nullptr &&
        m_captureOutput->GetRequiredSize() == bytes &&
        m_captureReadback->GetRequiredSize() == bytes;
    if (valid && m_surfaceOutput != nullptr)
    {
        valid = m_surfaceReadback != nullptr &&
            m_surfaceOutput->GetRequiredSize() == pixels * ZEN_SURFACE_CAPTURE_BYTES_PER_PIXEL &&
            m_surfaceReadback->GetRequiredSize() == m_surfaceOutput->GetRequiredSize();
    }
    HeapVector<ComputeDispatchChunk> chunks;
    uint32_t first = 0;
    while (valid && first < pixels)
    {
        ComputeDispatchChunk chunk;
        valid = BuildComputeDispatchChunk(first, static_cast<uint32_t>(pixels) - first,
                                          ZEN_LIGHTING_CAPTURE_GROUP_SIZE, gpu, chunk);
        if (valid)
        {
            chunks.push_back(chunk);
            first += chunk.itemCount;
        }
    }
    for (uint32_t surface = 0; valid && surface < (m_surfaceOutput != nullptr ? 2u : 1u); ++surface)
    {
        RDGComputePassDesc clear;
        clear.SetShaderProgramName(surface != 0 ? "ClearSurfaceCaptureSP" :
                                                  "ClearLightingCaptureSP");
        clear.SetPassTag("ClearLightingCapture");
        clear.independentDispatches = true;
        clear.BindStorageBuffer(surface != 0 ? "SurfaceCapture" : "LightingCapture",
                                surface != 0 ? m_surfaceOutput : m_captureOutput,
                                RDGContentGuarantee::eFullWrite);
        m_pRenderDevice->GetCurrentFrameRDG()
            ->AddComputePass(std::move(clear))
            .RecordPassCommands([chunks](RDGPassCmdEncoder& encoder) {
                RecordLightingCaptureClear(encoder, chunks);
            });
    }
    return valid;
}

void DeferredLightingRenderer::BuildRenderGraph(VoxelGIRenderer* voxelGI,
                                                SceneShadowRenderer* shadows)
{
    BuildGBufferGraph();
    BuildCompositionGraph(voxelGI, shadows);
}

void DeferredLightingRenderer::BuildGBufferGraph(bool dynamicGI)
{
    RenderGraph* pRDG = m_pRenderDevice->GetCurrentFrameRDG();
    VERIFY_EXPR(pRDG != nullptr && m_pScene != nullptr);
    {
        const uint32_t offscreenSize = RenderConfig::GetInstance().offScreenFbSize;

        RHIGfxPipelineStates pso{};
        pso.rasterizationState          = {};
        pso.rasterizationState.cullMode = RHIPolygonCullMode::eBack;

        pso.depthStencilState =
            RHIGfxPipelineDepthStencilState::Create(true, true, RHIDepthCompareOperator::eLess);
        pso.multiSampleState = {};
        pso.colorBlendState.AddAttachments(dynamicGI ? 7 : 5);
        pso.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);

        RDGGraphicsPassDesc offscreen{};
        offscreen.SetShaderProgramName(dynamicGI ? "GBufferDynamicSP" : "GBufferSP");
        offscreen.AddColorOutput(DataFormat::eR16G16B16A16SFloat, offscreenSize, offscreenSize,
                                 "offscreen_position");
        offscreen.AddColorOutput(DataFormat::eR16G16B16A16SFloat, offscreenSize, offscreenSize,
                                 "offscreen_normal");
        offscreen.AddColorOutput(DataFormat::eR8G8B8A8UNORM, offscreenSize, offscreenSize,
                                 "offscreen_albedo");
        offscreen.AddColorOutput(DataFormat::eR8G8B8A8UNORM, offscreenSize, offscreenSize,
                                 "offscreen_roughness");
        offscreen.AddColorOutput(DataFormat::eR16G16B16A16SFloat, offscreenSize, offscreenSize,
                                 "offscreen_emissive_occlusion");
        if (dynamicGI)
        {
            offscreen.AddColorOutput(DataFormat::eR32G32UInt, offscreenSize, offscreenSize,
                                     "offscreen_receiver");
            offscreen.AddColorOutput(DataFormat::eR16G16B16A16SFloat, offscreenSize, offscreenSize,
                                     "offscreen_geometric_normal");
        }
        offscreen.AddDepthStencilOutput(
            m_pViewport->GetDepthStencilFormat(), offscreenSize, offscreenSize, "offscreen_depth",
            RHIRenderTargetLoadOp::eClear, RHIRenderTargetStoreOp::eStore);
        offscreen.SetPipelineStates(pso);
        offscreen.SetRenderArea(0, 0, offscreenSize, offscreenSize);
        offscreen.SetPassTag("OffScreen");

        offscreen.BindStorageBuffer("NodeBuffer", m_pScene->GetNodesDataSSBO());
        offscreen.BindStorageBuffer("MaterialBuffer", m_pScene->GetMaterialsDataSSBO());
        BindSceneTextureArray(offscreen, m_pColorSampler, m_pScene->GetSceneTextures());

        offscreen.BindValue("uCameraData", m_pScene->GetCameraUniformData(),
                            sizeof(sg::CameraUniformData));
        offscreen.BindVertexBuffer(m_pScene->GetVertexBuffer());
        offscreen.BindIndexBuffer(m_pScene->GetIndexBuffer());

        pRDG->AddGraphicsPass(std::move(offscreen))
            .RecordPassCommands(
                [draws = SnapshotSceneDraws(*m_pScene), dynamicGI](RDGPassCmdEncoder& encoder) {
                    RecordGBufferDraws(encoder, draws, dynamicGI);
                });
    }
}

void DeferredLightingRenderer::BuildCompositionGraph(VoxelGIRenderer* voxelGI,
                                                     SceneShadowRenderer* shadows,
                                                     DynamicVoxelGIRenderer* dynamicGI)
{
    RenderGraph* pRDG = m_pRenderDevice->GetCurrentFrameRDG();
    VERIFY_EXPR(pRDG != nullptr && m_pScene != nullptr);
    m_captureRecorded       = false;
    const bool capture      = m_captureOutput != nullptr;
    const bool captureValid = !capture ||
        ((dynamicGI == nullptr || m_surfaceOutput != nullptr) && BuildLightingCaptureClear());
    const glm::uvec2 captureExtent(m_pViewport->GetWidth(), m_pViewport->GetHeight());


    {
        RHIGfxPipelineStates pso{};
        pso.primitiveType      = RHIDrawPrimitiveType::eTriangleList;
        pso.rasterizationState = {};
        pso.multiSampleState   = {};
        pso.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);

        pso.colorBlendState.AddAttachment();
        pso.depthStencilState = RHIGfxPipelineDepthStencilState::Create(
            true, true, RHIDepthCompareOperator::eLessOrEqual);

        RDGGraphicsPassDesc lighting{};
        lighting.SetShaderProgramName(
            dynamicGI != nullptr ?
                (capture ? "DeferredDynamicVoxelGICaptureSP" : "DeferredDynamicVoxelGISP") :
                voxelGI == nullptr ?
                (capture ? "DeferredLightingCaptureSP" : "DeferredLightingSP") :
                (capture ? "DeferredVoxelGICaptureSP" : "DeferredVoxelGISP"));
        lighting.AddColorOutput(m_pViewport->GetColorBackBuffer(), RHIRenderTargetLoadOp::eLoad);
        lighting.AddDepthStencilOutput(m_pViewport->GetDepthStencilBackBuffer(),
                                       RHIRenderTargetLoadOp::eClear,
                                       RHIRenderTargetStoreOp::eStore);
        lighting.SetPipelineStates(pso);
        lighting.SetRenderArea(0, 0, m_pViewport->GetWidth(), m_pViewport->GetHeight());
        lighting.SetPassTag("SceneLighting");

        const EnvTexture& env = m_pScene->GetEnvTexture();
        lighting.BindSampledTexture("positionMap", m_pColorSampler, "offscreen_position");
        lighting.BindSampledTexture("normalMap", m_pColorSampler, "offscreen_normal");
        lighting.BindSampledTexture("albedoMap", m_pColorSampler, "offscreen_albedo");
        lighting.BindSampledTexture("metallicRoughnessMap", m_pColorSampler, "offscreen_roughness");
        lighting.BindSampledTexture("emissiveOcclusionMap", m_pColorSampler,
                                    "offscreen_emissive_occlusion");
        lighting.BindSampledTexture("depthMap", m_pDepthSampler, "offscreen_depth");
        lighting.BindSampledTexture("envIrradianceMap", env.pIrradianceSampler,
                                    env.pIrradiance->GetDefaultView());
        lighting.BindSampledTexture("envPrefilteredMap", env.pPrefilteredSampler,
                                    env.pPrefiltered->GetDefaultView());
        lighting.BindSampledTexture("lutBRDFMap", env.pLutBRDFSampler,
                                    env.pLutBRDF->GetDefaultView());
        lighting.BindValue("uSceneData", m_pScene->GetSceneUniformData(), sizeof(SceneUniformData));
        if (capture)
        {
            lighting.BindStorageBuffer("LightingCapture", m_captureOutput);
            if (dynamicGI != nullptr)
            {
                lighting.BindStorageBuffer("SurfaceCapture", m_surfaceOutput);
            }
        }

        if (dynamicGI != nullptr)
        {
            dynamicGI->BindLightingInputs(lighting);
            lighting.BindValue("uSurfaceLookup",
                               glm::uvec4(m_pViewport->GetWidth(), m_pViewport->GetHeight(), 0, 0));
            lighting.BindSampledTexture("receiverMap", m_pDepthSampler, "offscreen_receiver");
            lighting.BindSampledTexture("geometricNormalMap", m_pColorSampler,
                                        "offscreen_geometric_normal");
        }
        if (voxelGI != nullptr)
        {
            voxelGI->BindLightingInputs(lighting);
            VERIFY_EXPR(shadows != nullptr);
            shadows->BindLightingInputs(lighting);
        }

        pRDG->AddGraphicsPass(std::move(lighting))
            .RecordPassCommands([capture, captureValid, captureExtent](RDGPassCmdEncoder& encoder) {
                if (!captureValid)
                {
                    encoder.Fail(RDGErrorCode::eRange,
                                 "Lighting capture extent, buffer size or device limits changed");
                }
                if (capture)
                {
                    encoder.SetPushConstants(captureExtent);
                }
                encoder.Draw(3, 1);
            });
    }
    if (capture && captureValid)
    {
        pRDG->AddTransferPass("ReadLightingCapture")
            .CopyBuffer(m_captureOutput, m_captureReadback,
                        {0, 0, m_captureOutput->GetRequiredSize()})
            .NeverCull();
        if (dynamicGI != nullptr)
        {
            pRDG->AddTransferPass("ReadSurfaceCapture")
                .CopyBuffer(m_surfaceOutput, m_surfaceReadback,
                            {0, 0, m_surfaceOutput->GetRequiredSize()})
                .NeverCull();
        }
        m_captureRecorded = true;
    }
    BuildLightMarkers();
}

void DeferredLightingRenderer::BuildLightMarkers()
{
    const SceneUniformData& sceneData =
        *reinterpret_cast<const SceneUniformData*>(m_pScene->GetSceneUniformData());
    const uint32_t lightCount = static_cast<uint32_t>(sceneData.lightInfo.x);
    if (m_lightMarkerSize > 0.0f && lightCount > 0)
    {
        RHIGfxPipelineStates pso{};
        pso.rasterizationState.cullMode = RHIPolygonCullMode::eDisabled;
        pso.depthStencilState =
            RHIGfxPipelineDepthStencilState::Create(true, true, RHIDepthCompareOperator::eLess);
        pso.colorBlendState.AddAttachment();
        pso.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);

        RDGGraphicsPassDesc markers;
        markers.SetShaderProgramName("LightMarkerSP");
        markers.SetPassTag("LightMarkers");
        markers.SetPipelineStates(pso);
        markers.SetRenderArea(0, 0, m_pViewport->GetWidth(), m_pViewport->GetHeight());
        markers.AddColorOutput(m_pViewport->GetColorBackBuffer(), RHIRenderTargetLoadOp::eLoad);
        markers.AddDepthStencilOutput(m_pViewport->GetDepthStencilBackBuffer(),
                                      RHIRenderTargetLoadOp::eLoad, RHIRenderTargetStoreOp::eStore);
        markers.BindValue("uCameraData", m_pScene->GetCameraUniformData(),
                          sizeof(sg::CameraUniformData));
        markers.BindValue("uSceneData", sceneData);
        // Visual-only boxes share the lighting snapshot and never enter the voxel volume.
        const float markerSize = m_lightMarkerSize;
        m_pRenderDevice->GetCurrentFrameRDG()
            ->AddGraphicsPass(std::move(markers))
            .RecordPassCommands([markerSize, lightCount](RDGPassCmdEncoder& encoder) {
                encoder.SetPushConstants(markerSize);
                encoder.Draw(36, lightCount);
            });
    }
}

} // namespace zen::rc
