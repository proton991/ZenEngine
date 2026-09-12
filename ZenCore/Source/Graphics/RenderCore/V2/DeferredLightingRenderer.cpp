#include "Graphics/RenderCore/V2/Renderer/DeferredLightingRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/RendererUtils.h"
#include "Graphics/RenderCore/V2/Renderer/SkyboxRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"
#include "Graphics/RenderCore/V2/RenderResource.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "SceneGraph/Camera.h"

namespace zen::rc
{
DeferredLightingRenderer::DeferredLightingRenderer(RenderDevice* pRenderDevice,
                                                   RHIViewport* pViewport) :
    m_pRenderDevice(pRenderDevice), m_pViewport(pViewport)
{}

void DeferredLightingRenderer::Init()
{
    PrepareSamplers();
}

void DeferredLightingRenderer::Destroy() {}

void DeferredLightingRenderer::PrepareSamplers()
{
    {
        RHISamplerCreateInfo samplerInfo{};
        samplerInfo.borderColor = RHISamplerBorderColor::eFloatOpaqueWhite;
        m_pDepthSampler         = m_pRenderDevice->CreateSampler(samplerInfo);
    }

    {
        RHISamplerCreateInfo samplerInfo{};
        samplerInfo.borderColor = RHISamplerBorderColor::eFloatOpaqueWhite;
        samplerInfo.minFilter   = RHISamplerFilter::eLinear;
        samplerInfo.magFilter   = RHISamplerFilter::eLinear;
        samplerInfo.mipFilter   = RHISamplerFilter::eLinear;
        samplerInfo.repeatU     = RHISamplerRepeatMode::eRepeat;
        samplerInfo.repeatV     = RHISamplerRepeatMode::eRepeat;
        samplerInfo.repeatW     = RHISamplerRepeatMode::eRepeat;
        samplerInfo.borderColor = RHISamplerBorderColor::eFloatOpaqueWhite;
        m_pColorSampler         = m_pRenderDevice->CreateSampler(samplerInfo);
    }
}

void DeferredLightingRenderer::BuildRenderGraph()
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
        pso.colorBlendState.AddAttachments(5);
        pso.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);

        RDGGraphicsPassDesc offscreen{};
        offscreen.SetShaderProgramName("GBufferSP");
        offscreen.AddColorOutput(DataFormat::eR16G16B16A16SFloat, offscreenSize, offscreenSize,
                                 "offscreen_position");
        offscreen.AddColorOutput(DataFormat::eR16G16B16A16SFloat, offscreenSize, offscreenSize,
                                 "offscreen_normal");
        offscreen.AddColorOutput(DataFormat::eR8G8B8A8UNORM, offscreenSize, offscreenSize,
                                 "offscreen_albedo");
        offscreen.AddColorOutput(DataFormat::eR8G8B8A8UNORM, offscreenSize, offscreenSize,
                                 "offscreen_roughness");
        offscreen.AddColorOutput(DataFormat::eR8G8B8A8UNORM, offscreenSize, offscreenSize,
                                 "offscreen_emissive_occlusion");
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
                [draws = SnapshotSceneDraws(*m_pScene)](RDGPassCmdEncoder& encoder) {
                    GBufferSP::PushConstantsData constants{};

                    for (SceneMeshDraw const& draw : draws)
                    {
                        constants.nodeIndex     = draw.nodeIndex;
                        constants.materialIndex = draw.materialIndex;
                        encoder.SetPushConstants(constants);
                        encoder.DrawIndexed(draw.indexCount, 1, draw.firstIndex, 0, 0);
                    }
                });
    }

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
        lighting.SetShaderProgramName("DeferredLightingSP");
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

        pRDG->AddGraphicsPass(std::move(lighting))
            .RecordPassCommands([](RDGPassCmdEncoder& encoder) { encoder.Draw(3, 1); });
    }
}

} // namespace zen::rc
