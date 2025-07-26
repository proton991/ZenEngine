#include "Graphics/RenderCore/V2/Renderer/ShadowMapRenderer.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RenderCore/V2/TextureManager.h"
#include "SceneGraph/Scene.h"

using namespace zen::rhi;

namespace zen::rc
{
ShadowMapRenderer::ShadowMapRenderer(RenderDevice* renderDevice, rhi::RHIViewport* viewport) :
    m_renderDevice(renderDevice), m_viewport(viewport)
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

void ShadowMapRenderer::Destroy() {}

void ShadowMapRenderer::SetRenderScene(RenderScene* renderScene)
{
    m_scene     = renderScene;
    m_lightView = sg::Camera::CreateOrthoOnAABB(m_scene->GetAABB());
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
    {
        TextureInfo texInfo{};
        texInfo.type   = rhi::TextureType::e2D;
        texInfo.format = m_config.shadowMapFormat;
        texInfo.width  = m_config.shadowMapWidth;
        texInfo.height = m_config.shadowMapHeight;
        texInfo.depth  = 1;
        texInfo.mipmaps =
            CalculateTextureMipLevels(m_config.shadowMapWidth, m_config.shadowMapHeight);
        texInfo.arrayLayers = 1;
        texInfo.samples     = SampleCount::e1;
        texInfo.usageFlags.SetFlags(
            TextureUsageFlagBits::eColorAttachment, TextureUsageFlagBits::eSampled,
            TextureUsageFlagBits::eTransferSrc, TextureUsageFlagBits::eTransferDst);
        m_offscreenTextures.shadowMap = m_renderDevice->CreateTexture(texInfo, "shadowmap");
    }
    // depth
    {

        rhi::TextureInfo texInfo{};
        texInfo.type        = rhi::TextureType::e2D;
        texInfo.format      = m_viewport->GetDepthStencilFormat();
        texInfo.type        = rhi::TextureType::e2D;
        texInfo.width       = m_config.shadowMapWidth;
        texInfo.height      = m_config.shadowMapHeight;
        texInfo.depth       = 1;
        texInfo.arrayLayers = 1;
        texInfo.mipmaps     = 1;
        texInfo.usageFlags.SetFlags(TextureUsageFlagBits::eDepthStencilAttachment,
                                    TextureUsageFlagBits::eSampled);
        m_offscreenTextures.depth =
            m_renderDevice->CreateTexture(texInfo, "shadowmap_render_depth");
    }
    {
        SamplerInfo samplerInfo{};
        samplerInfo.borderColor = SamplerBorderColor::eFloatOpaqueWhite;
        samplerInfo.minFilter   = rhi::SamplerFilter::eLinear;
        samplerInfo.magFilter   = rhi::SamplerFilter::eLinear;
        samplerInfo.mipFilter   = rhi::SamplerFilter::eLinear;
        samplerInfo.repeatU     = rhi::SamplerRepeatMode::eRepeat;
        samplerInfo.repeatV     = rhi::SamplerRepeatMode::eRepeat;
        samplerInfo.repeatW     = rhi::SamplerRepeatMode::eRepeat;
        samplerInfo.borderColor = SamplerBorderColor::eFloatOpaqueWhite;
        m_colorSampler          = m_renderDevice->CreateSampler(samplerInfo);
    }
}

void ShadowMapRenderer::BuildGraphicsPasses()
{
    {
        GfxPipelineStates pso{};
        pso.rasterizationState          = {};
        pso.rasterizationState.cullMode = PolygonCullMode::eDisabled;

        pso.depthStencilState =
            GfxPipelineDepthStencilState::Create(true, true, CompareOperator::eLess);
        pso.multiSampleState = {};
        pso.colorBlendState  = GfxPipelineColorBlendState::CreateDisabled(1);
        pso.dynamicStates.push_back(DynamicState::eScissor);
        pso.dynamicStates.push_back(DynamicState::eViewPort);

        rc::GraphicsPassBuilder builder(m_renderDevice);
        m_gfxPasses.evsm =
            builder.SetShaderProgramName("ShadowMapRenderSP")
                .SetNumSamples(SampleCount::e1)
                .AddColorRenderTarget(m_config.shadowMapFormat, TextureUsage::eColorAttachment,
                                      m_offscreenTextures.shadowMap)
                .SetDepthStencilTarget(m_viewport->GetDepthStencilFormat(),
                                       m_offscreenTextures.depth, rhi::RenderTargetLoadOp::eClear,
                                       rhi::RenderTargetStoreOp::eStore)
                .SetPipelineState(pso)
                .SetFramebufferInfo(m_viewport, m_config.shadowMapWidth, m_config.shadowMapHeight)
                .SetTag("evsm")
                .Build();
    }
}

void ShadowMapRenderer::BuildRenderGraph()
{
    m_rdg = MakeUnique<RenderGraph>();
    m_rdg->Begin();
    // offscreen pass
    {
        ShadowMapRenderSP* shaderProgram =
            dynamic_cast<ShadowMapRenderSP*>(m_gfxPasses.evsm.shaderProgram);
        std::vector<RenderPassClearValue> clearValues(2);
        clearValues[0].color   = {0.0f, 0.0f, 0.0f, 0.0f};
        clearValues[1].depth   = 1.0f;
        clearValues[1].stencil = 0;

        Rect2<int> area(0, static_cast<int>(m_config.shadowMapWidth), 0,
                        static_cast<int>(m_config.shadowMapHeight));
        Rect2<float> viewport(static_cast<float>(m_config.shadowMapWidth),
                              static_cast<float>(m_config.shadowMapHeight));

        auto* pass = m_rdg->AddGraphicsPassNode(m_gfxPasses.evsm, area, clearValues, true);
        m_rdg->DeclareTextureAccessForPass(
            pass, m_offscreenTextures.shadowMap, TextureUsage::eColorAttachment,
            m_renderDevice->GetTextureSubResourceRange(m_offscreenTextures.shadowMap),
            rhi::AccessMode::eReadWrite);
        m_rdg->DeclareTextureAccessForPass(
            pass, m_offscreenTextures.depth, TextureUsage::eDepthStencilAttachment,
            TextureSubResourceRange::DepthStencil(), rhi::AccessMode::eReadWrite);
        m_rdg->AddGraphicsPassBindVertexBufferNode(pass, m_scene->GetVertexBuffer(), {0});
        m_rdg->AddGraphicsPassBindIndexBufferNode(pass, m_scene->GetIndexBuffer(),
                                                  DataFormat::eR32UInt);
        m_rdg->AddGraphicsPassSetViewportNode(pass, viewport);
        m_rdg->AddGraphicsPassSetScissorNode(pass, area);
        shaderProgram->pushConstantsData.alphaCutoff = 0.01f;
        shaderProgram->pushConstantsData.exponents   = m_config.exponents;
        for (auto* node : m_scene->GetRenderableNodes())
        {
            shaderProgram->pushConstantsData.nodeIndex = node->GetRenderableIndex();
            for (auto* subMesh : node->GetComponent<sg::Mesh>()->GetSubMeshes())
            {
                shaderProgram->pushConstantsData.materialIndex = subMesh->GetMaterial()->index;
                m_rdg->AddGraphicsPassSetPushConstants(pass, &shaderProgram->pushConstantsData,
                                                       sizeof(ShadowMapRenderSP::PushConstantData));
                m_rdg->AddGraphicsPassDrawIndexedNode(pass, subMesh->GetIndexCount(), 1,
                                                      subMesh->GetFirstIndex(), 0, 0);
            }
        }
    }
    m_rdg->AddTextureMipmapGenNode(m_offscreenTextures.shadowMap);
    m_rdg->End();
}

void ShadowMapRenderer::UpdateGraphicsPassResources()
{
    {
        ShadowMapRenderSP* shaderProgram =
            dynamic_cast<ShadowMapRenderSP*>(m_gfxPasses.evsm.shaderProgram);
        std::vector<ShaderResourceBinding> set0bindings;
        std::vector<ShaderResourceBinding> set1bindings;
        // set-0 bindings
        ADD_SHADER_BINDING_SINGLE(set0bindings, 0, ShaderResourceType::eUniformBuffer,
                                  shaderProgram->GetUniformBufferHandle("uLightInfo"));
        ADD_SHADER_BINDING_SINGLE(set0bindings, 1, ShaderResourceType::eStorageBuffer,
                                  m_scene->GetNodesDataSSBO());
        ADD_SHADER_BINDING_SINGLE(set0bindings, 2, ShaderResourceType::eStorageBuffer,
                                  m_scene->GetMaterialsDataSSBO());

        // set-1 bindings
        // texture array
        ADD_SHADER_BINDING_TEXTURE_ARRAY(set1bindings, 0, ShaderResourceType::eSamplerWithTexture,
                                         m_colorSampler, m_scene->GetSceneTextures())

        rc::GraphicsPassResourceUpdater updater(m_renderDevice, &m_gfxPasses.evsm);
        updater.SetShaderResourceBinding(0, set0bindings)
            .SetShaderResourceBinding(1, set1bindings)
            .Update();
    }
}

void ShadowMapRenderer::UpdateUniformData()
{
    const auto* cameraUniformData =
        reinterpret_cast<const sg::CameraUniformData*>(m_scene->GetCameraUniformData());
    m_gfxPasses.evsm.shaderProgram->UpdateUniformBuffer(
        "uLightInfo", reinterpret_cast<const uint8_t*>(&cameraUniformData->projViewMatrix), 0);
}
} // namespace zen::rc