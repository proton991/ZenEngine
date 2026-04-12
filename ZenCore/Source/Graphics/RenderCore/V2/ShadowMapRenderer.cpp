#include "Graphics/RenderCore/V2/Renderer/ShadowMapRenderer.h"

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

    BuildGraphicsPasses();
}

void ShadowMapRenderer::Destroy()
{
    m_pRenderDevice->DestroyTexture(m_offscreenTextures.pShadowMap);
    m_pRenderDevice->DestroyTexture(m_offscreenTextures.pDepth);
}

void ShadowMapRenderer::SetRenderScene(RenderScene* pRenderScene)
{
    m_pScene     = pRenderScene;
    m_lightView = sg::Camera::CreateOrthoOnAABB(m_pScene->GetAABB());
    UpdateUniformData();
    UpdateGraphicsPassResources();
}

void ShadowMapRenderer::PrepareRenderWorkload()
{
    if (m_rebuildRDG)
    {
        BuildRenderGraph();
        m_rebuildRDG = false;
    }
}

void ShadowMapRenderer::PrepareTextures()
{
    TextureUsageHint usageHint{.copyUsage = false};
    {
        // TextureInfo texInfo{};
        // texInfo.type   = RHITextureType::e2D;
        // texInfo.format = m_config.shadowMapFormat;
        // texInfo.width  = m_config.shadowMapWidth;
        // texInfo.height = m_config.shadowMapHeight;
        // texInfo.depth  = 1;
        // texInfo.mipmaps =
        //     CalculateTextureMipLevels(m_config.shadowMapWidth, m_config.shadowMapHeight);
        // texInfo.arrayLayers = 1;
        // texInfo.samples     = SampleCount::e1;
        // texInfo.usageFlags.SetFlags(
        //     RHITextureUsageFlagBits::eColorAttachment, RHITextureUsageFlagBits::eSampled,
        //     RHITextureUsageFlagBits::eTransferSrc, RHITextureUsageFlagBits::eTransferDst);
        // texInfo.name                  = "shadowmap";
        // m_offscreenTextures.shadowMap = m_renderDevice->CreateTexture(texInfo);

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
            m_pRenderDevice->CreateTextureColorRT(texFormat, {.copyUsage = true}, "shadowmap");
    }
    // depth
    {

        // TextureInfo texInfo{};
        // texInfo.type        = RHITextureType::e2D;
        // texInfo.format      = m_viewport->GetDepthStencilFormat();
        // texInfo.type        = RHITextureType::e2D;
        // texInfo.width       = m_config.shadowMapWidth;
        // texInfo.height      = m_config.shadowMapHeight;
        // texInfo.depth       = 1;
        // texInfo.arrayLayers = 1;
        // texInfo.mipmaps     = 1;
        // texInfo.usageFlags.SetFlags(RHITextureUsageFlagBits::eDepthStencilAttachment,
        //                             RHITextureUsageFlagBits::eSampled);
        // texInfo.name              = "shadowmap_render_depth";
        // m_offscreenTextures.depth = m_renderDevice->CreateTexture(texInfo);

        TextureFormat texFormat{};
        texFormat.dimension   = TextureDimension::e2D;
        texFormat.format      = m_pViewport->GetDepthStencilFormat();
        texFormat.width       = m_config.shadowMapWidth;
        texFormat.height      = m_config.shadowMapHeight;
        texFormat.depth       = 1;
        texFormat.arrayLayers = 1;
        texFormat.mipmaps     = RHITexture::CalculateTextureMipLevels(m_config.shadowMapWidth,
                                                                      m_config.shadowMapHeight);

        m_offscreenTextures.pDepth = m_pRenderDevice->CreateTextureDepthStencilRT(
            texFormat, {.copyUsage = false}, "shadowmap_render_depth");
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
        m_pColorSampler          = m_pRenderDevice->CreateSampler(samplerInfo);
    }
}

void ShadowMapRenderer::BuildGraphicsPasses()
{
    {
        RHIGfxPipelineStates pso{};
        pso.rasterizationState          = {};
        pso.rasterizationState.cullMode = RHIPolygonCullMode::eDisabled;

        pso.depthStencilState =
            RHIGfxPipelineDepthStencilState::Create(true, true, RHIDepthCompareOperator::eLess);
        pso.multiSampleState = {};
        pso.colorBlendState.AddAttachment();
        pso.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);

        rc::GraphicsPassBuilder builder(m_pRenderDevice);
        m_gfxPasses.pEvsm =
            builder
                .SetShaderProgramName("ShadowMapRenderSP")
                // .SetNumSamples(SampleCount::e1)
                .AddColorRenderTarget(m_offscreenTextures.pShadowMap)
                .SetDepthStencilTarget(m_offscreenTextures.pDepth, RHIRenderTargetLoadOp::eClear,
                                       RHIRenderTargetStoreOp::eStore)
                .SetPipelineState(pso)
                .SetFramebufferInfo(m_pViewport, m_config.shadowMapWidth, m_config.shadowMapHeight)
                .SetTag("evsm")
                .Build();
    }
}

void ShadowMapRenderer::BuildRenderGraph()
{
    m_rdg = MakeUnique<RenderGraph>("shadowmap_rdg");
    m_rdg->Begin();
    // offscreen pPass
    {
        ShadowMapRenderSP* pShaderProgram =
            dynamic_cast<ShadowMapRenderSP*>(m_gfxPasses.pEvsm->pShaderProgram);
        // std::vector<RHIRenderPassClearValue> clearValues(2);
        // clearValues[0].color   = {0.0f, 0.0f, 0.0f, 0.0f};
        // clearValues[1].depth   = 1.0f;
        // clearValues[1].stencil = 0;

        Rect2<int> area(0, static_cast<int>(m_config.shadowMapWidth), 0,
                        static_cast<int>(m_config.shadowMapHeight));
        Rect2<float> viewport(static_cast<float>(m_config.shadowMapWidth),
                              static_cast<float>(m_config.shadowMapHeight));

        auto* pPass = m_rdg->AddGraphicsPassNode(m_gfxPasses.pEvsm, "shadowmap_offscreen");
        // m_rdg->DeclareTextureAccessForPass(
        //     pPass, m_offscreenTextures.shadowMap, RHITextureUsage::eColorAttachment,
        //     m_renderDevice->GetTextureSubResourceRange(m_offscreenTextures.shadowMap),
        //     RHIAccessMode::eReadWrite);
        // m_rdg->DeclareTextureAccessForPass(
        //     pPass, m_offscreenTextures.depth, RHITextureUsage::eDepthStencilAttachment,
        //     RHITextureSubResourceRange::DepthStencil(), RHIAccessMode::eReadWrite);
        m_rdg->AddGraphicsPassBindVertexBufferNode(pPass, m_pScene->GetVertexBuffer(), {0});
        m_rdg->AddGraphicsPassBindIndexBufferNode(pPass, m_pScene->GetIndexBuffer(),
                                                  DataFormat::eR32UInt);
        m_rdg->AddGraphicsPassSetViewportNode(pPass, viewport);
        m_rdg->AddGraphicsPassSetScissorNode(pPass, area);
        pShaderProgram->pushConstantsData.alphaCutoff = 0.01f;
        pShaderProgram->pushConstantsData.exponents   = m_config.exponents;
        for (auto* node : m_pScene->GetRenderableNodes())
        {
            pShaderProgram->pushConstantsData.nodeIndex = node->GetRenderableIndex();
            for (auto* subMesh : node->GetComponent<sg::Mesh>()->GetSubMeshes())
            {
                pShaderProgram->pushConstantsData.materialIndex = subMesh->GetMaterial()->index;
                m_rdg->AddGraphicsPassSetPushConstants(
                    pPass, &pShaderProgram->pushConstantsData,
                    sizeof(ShadowMapRenderSP::PushConstantsData));
                m_rdg->AddGraphicsPassDrawIndexedNode(pPass, subMesh->GetIndexCount(), 1,
                                                      subMesh->GetFirstIndex(), 0, 0);
            }
        }
    }
    m_rdg->AddTextureMipmapGenNode(m_offscreenTextures.pShadowMap);
    m_rdg->End();
}

void ShadowMapRenderer::UpdateGraphicsPassResources()
{
    {
        ShadowMapRenderSP* pShaderProgram =
            dynamic_cast<ShadowMapRenderSP*>(m_gfxPasses.pEvsm->pShaderProgram);
        HeapVector<RHIShaderResourceBinding> set0bindings;
        HeapVector<RHIShaderResourceBinding> set1bindings;
        // set-0 bindings
        ADD_SHADER_BINDING_SINGLE(set0bindings, 0, RHIShaderResourceType::eUniformBuffer,
                                  pShaderProgram->GetUniformBufferHandle("uLightInfo"));
        ADD_SHADER_BINDING_SINGLE(set0bindings, 1, RHIShaderResourceType::eStorageBuffer,
                                  m_pScene->GetNodesDataSSBO());
        ADD_SHADER_BINDING_SINGLE(set0bindings, 2, RHIShaderResourceType::eStorageBuffer,
                                  m_pScene->GetMaterialsDataSSBO());

        // set-1 bindings
        // texture array
        ADD_SHADER_BINDING_TEXTURE_ARRAY(set1bindings, 0,
                                         RHIShaderResourceType::eSamplerWithTexture, m_pColorSampler,
                                         m_pScene->GetSceneTextures())

        rc::GraphicsPassResourceUpdater updater(m_pRenderDevice, m_gfxPasses.pEvsm);
        updater.SetShaderResourceBinding(0, std::move(set0bindings))
            .SetShaderResourceBinding(1, std::move(set1bindings))
            .Update();
    }
}

void ShadowMapRenderer::UpdateUniformData()
{
    const auto* pCameraUniformData =
        reinterpret_cast<const sg::CameraUniformData*>(m_pScene->GetCameraUniformData());
    m_gfxPasses.pEvsm->pShaderProgram->UpdateUniformBuffer(
        "uLightInfo", reinterpret_cast<const uint8_t*>(&pCameraUniformData->projViewMatrix), 0);
}
} // namespace zen::rc