#include "Graphics/RenderCore/V2/Renderer/ShadowMapRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/RendererUtils.h"

#include "Graphics/RenderCore/V2/RenderResource.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RenderCore/V2/TextureManager.h"
#include "SceneGraph/Scene.h"

namespace zen::rc
{
ShadowMapRenderer::ShadowMapRenderer(RenderDevice* pRenderDevice, RHIViewport* pViewport) :
    m_pRenderDevice(pRenderDevice), m_pViewport(pViewport)
{}

void ShadowMapRenderer::Init()
{
    m_config.exponents       = Vec2(40.0f, 5.0f);
    m_config.shadowMapFormat = DataFormat::eR32G32B32A32SFloat;
    m_config.shadowMapWidth  = 1024;
    m_config.shadowMapHeight = 1024;

    PrepareTextures();
}

void ShadowMapRenderer::Destroy()
{
    m_pRenderDevice->DestroyTexture(m_offscreenTextures.pShadowMap);
    m_pRenderDevice->DestroyTexture(m_offscreenTextures.pShadowMapRenderTarget);
    m_pRenderDevice->DestroyTexture(m_offscreenTextures.pDepth);
}

void ShadowMapRenderer::SetRenderScene(RenderScene* pRenderScene)
{
    m_pScene = pRenderScene;
}

void ShadowMapRenderer::PrepareTextures()
{
    {
        TextureFormat texFormat{};
        texFormat.dimension   = TextureDimension::e2D;
        texFormat.format      = m_config.shadowMapFormat;
        texFormat.width       = m_config.shadowMapWidth;
        texFormat.height      = m_config.shadowMapHeight;
        texFormat.depth       = 1;
        texFormat.arrayLayers = 1;
        texFormat.mipmaps     = RHITexture::CalculateTextureMipLevels(m_config.shadowMapWidth,
                                                                      m_config.shadowMapHeight);

        m_offscreenTextures.pShadowMap =
            m_pRenderDevice->CreateTextureSampled(texFormat, {.copyUsage = true}, "shadowmap");

        // Rendering attachments use a single mip; copy the result into the sampled mip chain.
        texFormat.mipmaps                          = 1;
        m_offscreenTextures.pShadowMapRenderTarget = m_pRenderDevice->CreateTextureColorRT(
            texFormat, {.copyUsage = true}, "shadowmap_render_target");
    }

    {
        TextureFormat texFormat{};
        texFormat.dimension   = TextureDimension::e2D;
        texFormat.format      = m_pViewport->GetDepthStencilFormat();
        texFormat.width       = m_config.shadowMapWidth;
        texFormat.height      = m_config.shadowMapHeight;
        texFormat.depth       = 1;
        texFormat.arrayLayers = 1;
        texFormat.mipmaps     = 1;

        m_offscreenTextures.pDepth = m_pRenderDevice->CreateTextureDepthStencilRT(
            texFormat, {.copyUsage = false}, "shadowmap_render_depth");
    }

    const RHISamplerCreateInfo colorSamplerInfo = RHISamplerCreateInfo::CreateLinearRepeat();
    m_pColorSampler                             = m_pRenderDevice->CreateSampler(colorSamplerInfo);
}

void ShadowMapRenderer::BuildRenderGraph()
{
    RenderGraph* pRDG = m_pRenderDevice->GetCurrentFrameRDG();
    VERIFY_EXPR(pRDG != nullptr);

    RHIGfxPipelineStates pso{};
    pso.rasterizationState          = {};
    pso.rasterizationState.cullMode = RHIPolygonCullMode::eDisabled;

    pso.depthStencilState =
        RHIGfxPipelineDepthStencilState::Create(true, true, RHIDepthCompareOperator::eLess);
    pso.multiSampleState = {};
    pso.colorBlendState.AddAttachment();
    pso.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);

    RDGGraphicsPassDesc desc{};
    desc.SetShaderProgramName("ShadowMapRenderSP");
    desc.AddColorOutput(m_offscreenTextures.pShadowMapRenderTarget);
    desc.AddDepthStencilOutput(m_offscreenTextures.pDepth, RHIRenderTargetLoadOp::eClear,
                               RHIRenderTargetStoreOp::eStore);
    desc.SetPipelineStates(pso);
    desc.SetRenderArea(0, 0, m_config.shadowMapWidth, m_config.shadowMapHeight);
    desc.SetPassTag("evsm");

    desc.BindStorageBuffer("NodeBuffer", m_pScene->GetNodesDataSSBO());
    desc.BindStorageBuffer("MaterialBuffer", m_pScene->GetMaterialsDataSSBO());
    BindSceneTextureArray(desc, m_pColorSampler, m_pScene->GetSceneTextures());

    const sg::CameraUniformData* camera =
        reinterpret_cast<const sg::CameraUniformData*>(m_pScene->GetCameraUniformData());
    desc.BindValue("uLightInfo", camera->projViewMatrix);
    desc.BindVertexBuffer(m_pScene->GetVertexBuffer());
    desc.BindIndexBuffer(m_pScene->GetIndexBuffer());

    pRDG->AddGraphicsPass(std::move(desc))
        .RecordPassCommands([draws     = SnapshotSceneDraws(*m_pScene),
                             exponents = m_config.exponents](RDGPassCmdEncoder& encoder) {
            ShadowMapRenderSP::PushConstantsData constants{};
            constants.alphaCutoff = 0.01f;
            constants.exponents   = exponents;

            for (SceneMeshDraw const& draw : draws)
            {
                constants.nodeIndex     = draw.nodeIndex;
                constants.materialIndex = draw.materialIndex;
                encoder.SetPushConstants(constants);
                encoder.DrawIndexed(draw.indexCount, 1, draw.firstIndex, 0, 0);
            }
        });

    RHITextureCopyRegion region{};
    region.srcSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    region.dstSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
    region.size = {m_config.shadowMapWidth, m_config.shadowMapHeight, 1};

    pRDG->AddTransferPass("shadowmap_copy")
        .CopyTexture(m_offscreenTextures.pShadowMapRenderTarget, m_offscreenTextures.pShadowMap,
                     MakeVecView(&region, 1));

    pRDG->AddTransferPass("shadowmap_mipmaps").GenerateMipmaps(m_offscreenTextures.pShadowMap);
}

} // namespace zen::rc
