#include "Graphics/RenderCore/V2/Renderer/DeferredLightingRenderer.h"
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
    PrepareTextures();

    BuildGraphicsPasses();
}

void DeferredLightingRenderer::Destroy()
{
    m_pRenderDevice->DestroyTexture(m_offscreenTextures.pPosition);
    m_pRenderDevice->DestroyTexture(m_offscreenTextures.pNormal);
    m_pRenderDevice->DestroyTexture(m_offscreenTextures.pAlbedo);
    m_pRenderDevice->DestroyTexture(m_offscreenTextures.pMetallicRoughness);
    m_pRenderDevice->DestroyTexture(m_offscreenTextures.pEmissiveOcclusion);
    m_pRenderDevice->DestroyTexture(m_offscreenTextures.pDepth);
}

void DeferredLightingRenderer::PrepareRenderWorkload()
{
    if (m_rebuildRDG)
    {
        BuildRenderGraph();
        m_rebuildRDG = false;
    }
    m_gfxPasses.pOffscreen->pShaderProgram->UpdateUniformBuffer("uCameraData",
                                                              m_pScene->GetCameraUniformData(), 0);
    m_gfxPasses.pSceneLighting->pShaderProgram->UpdateUniformBuffer(
        "uSceneData", m_pScene->GetSceneUniformData(), 0);
}

void DeferredLightingRenderer::OnResize()
{
    m_rebuildRDG = true;
    m_pRenderDevice->UpdateGraphicsPassOnResize(m_gfxPasses.pSceneLighting, m_pViewport);
}

void DeferredLightingRenderer::PrepareTextures()
{
    TextureUsageHint usageHint{.copyUsage = false};
    // (World space) Positions
    {
        // TextureInfo texInfo{};
        // texInfo.type        = RHITextureType::e2D;
        // texInfo.format      = DataFormat::eR16G16B16A16SFloat;
        // texInfo.width       = RenderConfig::GetInstance().offScreenFbSize;
        // texInfo.height      = RenderConfig::GetInstance().offScreenFbSize;
        // texInfo.depth       = 1;
        // texInfo.mipmaps     = 1;
        // texInfo.arrayLayers = 1;
        // texInfo.samples     = SampleCount::e1;
        // texInfo.name        = "offscreen_position";
        // texInfo.usageFlags.SetFlags(RHITextureUsageFlagBits::eColorAttachment,
        //                             RHITextureUsageFlagBits::eSampled);
        // m_offscreenTextures.pPosition = m_renderDevice->CreateTexture(texInfo);

        TextureFormat texFormat{};
        texFormat.dimension   = TextureDimension::e2D;
        texFormat.format      = DataFormat::eR16G16B16A16SFloat;
        texFormat.width       = RenderConfig::GetInstance().offScreenFbSize;
        texFormat.height      = RenderConfig::GetInstance().offScreenFbSize;
        texFormat.depth       = 1;
        texFormat.arrayLayers = 1;
        texFormat.mipmaps     = 1;

        m_offscreenTextures.pPosition =
            m_pRenderDevice->CreateTextureColorRT(texFormat, usageHint, "offscreen_position");
    }
    // (World space) Normals
    {
        // TextureInfo texInfo{};
        // texInfo.type        = RHITextureType::e2D;
        // texInfo.format      = DataFormat::eR16G16B16A16SFloat;
        // texInfo.width       = RenderConfig::GetInstance().offScreenFbSize;
        // texInfo.height      = RenderConfig::GetInstance().offScreenFbSize;
        // texInfo.depth       = 1;
        // texInfo.mipmaps     = 1;
        // texInfo.arrayLayers = 1;
        // texInfo.samples     = SampleCount::e1;
        // texInfo.name        = "offscreen_normal";
        // texInfo.usageFlags.SetFlags(RHITextureUsageFlagBits::eColorAttachment,
        //                             RHITextureUsageFlagBits::eSampled);
        // m_offscreenTextures.pNormal = m_renderDevice->CreateTexture(texInfo);

        TextureFormat texFormat{};
        texFormat.dimension   = TextureDimension::e2D;
        texFormat.format      = DataFormat::eR16G16B16A16SFloat;
        texFormat.width       = RenderConfig::GetInstance().offScreenFbSize;
        texFormat.height      = RenderConfig::GetInstance().offScreenFbSize;
        texFormat.depth       = 1;
        texFormat.arrayLayers = 1;
        texFormat.mipmaps     = 1;

        m_offscreenTextures.pNormal =
            m_pRenderDevice->CreateTextureColorRT(texFormat, usageHint, "offscreen_normal");
    }
    // color
    {
        // TextureInfo texInfo{};
        // texInfo.type        = RHITextureType::e2D;
        // texInfo.format      = DataFormat::eR8G8B8A8UNORM;
        // texInfo.width       = RenderConfig::GetInstance().offScreenFbSize;
        // texInfo.height      = RenderConfig::GetInstance().offScreenFbSize;
        // texInfo.depth       = 1;
        // texInfo.mipmaps     = 1;
        // texInfo.arrayLayers = 1;
        // texInfo.samples     = SampleCount::e1;
        // texInfo.usageFlags.SetFlags(RHITextureUsageFlagBits::eColorAttachment,
        //                             RHITextureUsageFlagBits::eSampled);
        //
        // texInfo.name               = "offscreen_albedo";
        // m_offscreenTextures.pAlbedo = m_renderDevice->CreateTexture(texInfo);
        //
        // texInfo.name                          = "offscreen_roughness";
        // m_offscreenTextures.pMetallicRoughness = m_renderDevice->CreateTexture(texInfo);
        //
        // texInfo.name                          = "offscreen_emissive_occlusion";
        // m_offscreenTextures.pEmissiveOcclusion = m_renderDevice->CreateTexture(texInfo);

        TextureFormat texFormat{};
        texFormat.dimension   = TextureDimension::e2D;
        texFormat.format      = DataFormat::eR8G8B8A8UNORM;
        texFormat.width       = RenderConfig::GetInstance().offScreenFbSize;
        texFormat.height      = RenderConfig::GetInstance().offScreenFbSize;
        texFormat.depth       = 1;
        texFormat.arrayLayers = 1;
        texFormat.mipmaps     = 1;

        m_offscreenTextures.pAlbedo =
            m_pRenderDevice->CreateTextureColorRT(texFormat, usageHint, "offscreen_albedo");

        m_offscreenTextures.pMetallicRoughness =
            m_pRenderDevice->CreateTextureColorRT(texFormat, usageHint, "offscreen_roughness");

        m_offscreenTextures.pEmissiveOcclusion = m_pRenderDevice->CreateTextureColorRT(
            texFormat, usageHint, "offscreen_emissive_occlusion");
    }
    // depth
    {
        // TextureInfo texInfo{};
        // texInfo.type        = RHITextureType::e2D;
        // texInfo.format      = m_viewport->GetDepthStencilFormat();
        // texInfo.type        = RHITextureType::e2D;
        // texInfo.width       = RenderConfig::GetInstance().offScreenFbSize;
        // texInfo.height      = RenderConfig::GetInstance().offScreenFbSize;
        // texInfo.depth       = 1;
        // texInfo.arrayLayers = 1;
        // texInfo.mipmaps     = 1;
        // texInfo.name        = "offscreen_depth";
        // texInfo.usageFlags.SetFlags(RHITextureUsageFlagBits::eDepthStencilAttachment,
        //                             RHITextureUsageFlagBits::eSampled);
        // m_offscreenTextures.pDepth = m_renderDevice->CreateTexture(texInfo);

        TextureFormat texFormat{};
        texFormat.dimension   = TextureDimension::e2D;
        texFormat.format      = m_pViewport->GetDepthStencilFormat();
        texFormat.width       = RenderConfig::GetInstance().offScreenFbSize;
        texFormat.height      = RenderConfig::GetInstance().offScreenFbSize;
        texFormat.depth       = 1;
        texFormat.arrayLayers = 1;
        texFormat.mipmaps     = 1;

        m_offscreenTextures.pDepth =
            m_pRenderDevice->CreateTextureDepthStencilRT(texFormat, usageHint, "offscreen_depth");
    }
    // offscreen color texture sampler
    {
        RHISamplerCreateInfo samplerInfo{};
        samplerInfo.borderColor = RHISamplerBorderColor::eFloatOpaqueWhite;
        m_pDepthSampler          = m_pRenderDevice->CreateSampler(samplerInfo);
    }
    // offscreen depth texture sampler
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

void DeferredLightingRenderer::BuildGraphicsPasses()
{
    // offscreen
    {
        RHIGfxPipelineStates pso{};
        pso.rasterizationState          = {};
        pso.rasterizationState.cullMode = RHIPolygonCullMode::eBack;

        pso.depthStencilState =
            RHIGfxPipelineDepthStencilState::Create(true, true, RHIDepthCompareOperator::eLess);
        pso.multiSampleState = {};
        pso.colorBlendState.AddAttachments(5);
        pso.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);

        rc::GraphicsPassBuilder builder(m_pRenderDevice);
        m_gfxPasses.pOffscreen =
            builder
                .SetShaderProgramName("GBufferSP")
                // .SetNumSamples(SampleCount::e1)
                // (World space) Positions
                .AddColorRenderTarget(m_offscreenTextures.pPosition)
                // (World space) Normals
                .AddColorRenderTarget(m_offscreenTextures.pNormal)
                // Albedo (color)
                .AddColorRenderTarget(m_offscreenTextures.pAlbedo)
                // metallicRoughness
                .AddColorRenderTarget(m_offscreenTextures.pMetallicRoughness)
                // emissiveOcclusion
                .AddColorRenderTarget(m_offscreenTextures.pEmissiveOcclusion)
                .SetDepthStencilTarget(m_offscreenTextures.pDepth, RHIRenderTargetLoadOp::eClear,
                                       RHIRenderTargetStoreOp::eStore)
                .SetPipelineState(pso)
                .SetFramebufferInfo(m_pViewport, RenderConfig::GetInstance().offScreenFbSize,
                                    RenderConfig::GetInstance().offScreenFbSize)
                .SetTag("OffScreen")
                .Build();
    }

    // scene lighting
    {
        RHIGfxPipelineStates pso{};
        pso.primitiveType      = RHIDrawPrimitiveType::eTriangleList;
        pso.rasterizationState = {};
        pso.multiSampleState   = {};
        pso.dynamicStates.Enable(RHIDynamicState::eScissor, RHIDynamicState::eViewPort);

        pso.colorBlendState.AddAttachment();
        pso.depthStencilState = RHIGfxPipelineDepthStencilState::Create(
            true, true, RHIDepthCompareOperator::eLessOrEqual);

        rc::GraphicsPassBuilder builder(m_pRenderDevice);
        m_gfxPasses.pSceneLighting =
            builder
                .SetShaderProgramName("DeferredLightingSP")
                // .SetNumSamples(SampleCount::e1)
                .AddViewportColorRT(m_pViewport, RHIRenderTargetLoadOp::eLoad)
                .SetViewportDepthStencilRT(m_pViewport, RHIRenderTargetLoadOp::eClear,
                                           RHIRenderTargetStoreOp::eStore)
                .SetPipelineState(pso)
                .SetFramebufferInfo(m_pViewport)
                .SetTag("SceneLighting")
                .Build();
    }
}

void DeferredLightingRenderer::BuildRenderGraph()
{
    m_rdg = MakeUnique<RenderGraph>("deferred_lighting_rdg");
    m_rdg->Begin();
    // offscreen pPass
    {
        const uint32_t cFbSize = RenderConfig::GetInstance().offScreenFbSize;
        // std::vector<RHIRenderPassClearValue> clearValues(6);
        // clearValues[0].color   = {0.0f, 0.0f, 0.0f, 0.0f};
        // clearValues[1].color   = {0.0f, 0.0f, 0.0f, 0.0f};
        // clearValues[2].color   = {0.0f, 0.0f, 0.0f, 0.0f};
        // clearValues[3].color   = {0.0f, 0.0f, 0.0f, 0.0f};
        // clearValues[4].color   = {0.0f, 0.0f, 0.0f, 0.0f};
        // clearValues[5].depth   = 1.0f;
        // clearValues[5].stencil = 0;

        Rect2i area;
        area.minX = 0;
        area.minY = 0;
        area.maxX = static_cast<int>(cFbSize);
        area.maxY = static_cast<int>(cFbSize);

        Rect2<float> vp;
        vp.minX = 0.0f;
        vp.minY = 0.0f;
        vp.maxX = static_cast<float>(cFbSize);
        vp.maxY = static_cast<float>(cFbSize);

        auto* pPass = m_rdg->AddGraphicsPassNode(m_gfxPasses.pOffscreen, "offscreen_gbuffer");
        // m_rdg->DeclareTextureAccessForPass(
        //     pPass, m_offscreenTextures.pPosition, RHITextureUsage::eColorAttachment,
        //     RHITextureSubResourceRange::Color(), RHIAccessMode::eReadWrite);
        // m_rdg->DeclareTextureAccessForPass(
        //     pPass, m_offscreenTextures.pNormal, RHITextureUsage::eColorAttachment,
        //     RHITextureSubResourceRange::Color(), RHIAccessMode::eReadWrite);
        // m_rdg->DeclareTextureAccessForPass(
        //     pPass, m_offscreenTextures.pAlbedo, RHITextureUsage::eColorAttachment,
        //     RHITextureSubResourceRange::Color(), RHIAccessMode::eReadWrite);
        // m_rdg->DeclareTextureAccessForPass(
        //     pPass, m_offscreenTextures.pMetallicRoughness, RHITextureUsage::eColorAttachment,
        //     RHITextureSubResourceRange::Color(), RHIAccessMode::eReadWrite);
        // m_rdg->DeclareTextureAccessForPass(
        //     pPass, m_offscreenTextures.pEmissiveOcclusion, RHITextureUsage::eColorAttachment,
        //     RHITextureSubResourceRange::Color(), RHIAccessMode::eReadWrite);
        // m_rdg->DeclareTextureAccessForPass(
        //     pPass, m_offscreenTextures.pDepth, RHITextureUsage::eDepthStencilAttachment,
        //     RHITextureSubResourceRange::DepthStencil(), RHIAccessMode::eReadWrite);
        AddMeshDrawNodes(pPass, area, vp);
    }
    // scene lighting pPass
    {
        // std::vector<RHIRenderPassClearValue> clearValues(2);
        // clearValues[0].color   = {0.8f, 0.8f, 0.8f, 1.0f};
        // clearValues[1].depth   = 1.0f;
        // clearValues[1].stencil = 0;

        Rect2i area;
        area.minX = 0;
        area.minY = 0;
        area.maxX = static_cast<int>(m_pViewport->GetWidth());
        area.maxY = static_cast<int>(m_pViewport->GetHeight());

        Rect2<float> vp;
        vp.minX = 0.0f;
        vp.minY = 0.0f;
        vp.maxX = static_cast<float>(m_pViewport->GetWidth());
        vp.maxY = static_cast<float>(m_pViewport->GetHeight());

        auto* pPass = m_rdg->AddGraphicsPassNode(m_gfxPasses.pSceneLighting, "deferred_lighting");
        // m_rdg->DeclareTextureAccessForPass(pPass, m_offscreenTextures.pPosition,
        //                                    RHITextureUsage::eSampled, RHITextureSubResourceRange::Color(),
        //                                    RHIAccessMode::eRead);
        // m_rdg->DeclareTextureAccessForPass(pPass, m_offscreenTextures.pNormal, RHITextureUsage::eSampled,
        //                                    RHITextureSubResourceRange::Color(),
        //                                    RHIAccessMode::eRead);
        // m_rdg->DeclareTextureAccessForPass(pPass, m_offscreenTextures.pAlbedo, RHITextureUsage::eSampled,
        //                                    RHITextureSubResourceRange::Color(),
        //                                    RHIAccessMode::eRead);
        // m_rdg->DeclareTextureAccessForPass(pPass, m_offscreenTextures.pMetallicRoughness,
        //                                    RHITextureUsage::eSampled, RHITextureSubResourceRange::Color(),
        //                                    RHIAccessMode::eRead);
        // m_rdg->DeclareTextureAccessForPass(pPass, m_offscreenTextures.pEmissiveOcclusion,
        //                                    RHITextureUsage::eSampled, RHITextureSubResourceRange::Color(),
        //                                    RHIAccessMode::eRead);
        // m_rdg->DeclareTextureAccessForPass(pPass, m_offscreenTextures.pDepth, RHITextureUsage::eSampled,
        //                                    RHITextureSubResourceRange::DepthStencil(),
        //                                    RHIAccessMode::eRead);
        // Final composition
        // This is done by simply drawing a full screen quad
        // The fragment shader then combines the deferred attachments into the final image
        m_rdg->AddGraphicsPassSetScissorNode(pPass, area);
        m_rdg->AddGraphicsPassSetViewportNode(pPass, vp);
        m_rdg->AddGraphicsPassDrawNode(pPass, 3, 1);
    }
    m_rdg->End();
}

void DeferredLightingRenderer::AddMeshDrawNodes(RDGPassNode* pPass,
                                                const Rect2<int>& area,
                                                const Rect2<float>& viewport)
{
    GBufferSP* pShaderProgram = dynamic_cast<GBufferSP*>(m_gfxPasses.pOffscreen->pShaderProgram);
    m_rdg->AddGraphicsPassBindVertexBufferNode(pPass, m_pScene->GetVertexBuffer(), {0});
    m_rdg->AddGraphicsPassBindIndexBufferNode(pPass, m_pScene->GetIndexBuffer(),
                                              DataFormat::eR32UInt);
    m_rdg->AddGraphicsPassSetViewportNode(pPass, viewport);
    m_rdg->AddGraphicsPassSetScissorNode(pPass, area);
    for (auto* node : m_pScene->GetRenderableNodes())
    {
        pShaderProgram->pushConstantsData.nodeIndex = node->GetRenderableIndex();
        for (auto* subMesh : node->GetComponent<sg::Mesh>()->GetSubMeshes())
        {
            pShaderProgram->pushConstantsData.materialIndex = subMesh->GetMaterial()->index;
            m_rdg->AddGraphicsPassSetPushConstants(pPass, &pShaderProgram->pushConstantsData,
                                                   sizeof(GBufferSP::PushConstantsData));
            m_rdg->AddGraphicsPassDrawIndexedNode(pPass, subMesh->GetIndexCount(), 1,
                                                  subMesh->GetFirstIndex(), 0, 0);
        }
    }
}

void DeferredLightingRenderer::UpdateGraphicsPassResources()
{
    const EnvTexture& envTexture = m_pScene->GetEnvTexture();
    {
        HeapVector<RHIShaderResourceBinding> bufferBindings;
        HeapVector<RHIShaderResourceBinding> textureBindings;
        // buffers
        ADD_SHADER_BINDING_SINGLE(
            bufferBindings, 0, RHIShaderResourceType::eUniformBuffer,
            m_gfxPasses.pOffscreen->pShaderProgram->GetUniformBufferHandle("uCameraData"));
        ADD_SHADER_BINDING_SINGLE(bufferBindings, 1, RHIShaderResourceType::eStorageBuffer,
                                  m_pScene->GetNodesDataSSBO());
        ADD_SHADER_BINDING_SINGLE(bufferBindings, 2, RHIShaderResourceType::eStorageBuffer,
                                  m_pScene->GetMaterialsDataSSBO());
        // texture array
        ADD_SHADER_BINDING_TEXTURE_ARRAY(textureBindings, 0,
                                         RHIShaderResourceType::eSamplerWithTexture, m_pColorSampler,
                                         m_pScene->GetSceneTextures())

        GraphicsPassResourceUpdater updater(m_pRenderDevice, m_gfxPasses.pOffscreen);
        updater.SetShaderResourceBinding(0, std::move(bufferBindings))
            .SetShaderResourceBinding(1, std::move(textureBindings))
            .Update();
    }
    {
        HeapVector<RHIShaderResourceBinding> bufferBindings;
        HeapVector<RHIShaderResourceBinding> textureBindings;
        // buffer
        ADD_SHADER_BINDING_SINGLE(
            bufferBindings, 0, RHIShaderResourceType::eUniformBuffer,
            m_gfxPasses.pSceneLighting->pShaderProgram->GetUniformBufferHandle("uSceneData"));
        // textures
        ADD_SHADER_BINDING_SINGLE(textureBindings, 0, RHIShaderResourceType::eSamplerWithTexture,
                                  m_pColorSampler, m_offscreenTextures.pPosition);
        ADD_SHADER_BINDING_SINGLE(textureBindings, 1, RHIShaderResourceType::eSamplerWithTexture,
                                  m_pColorSampler, m_offscreenTextures.pNormal);
        ADD_SHADER_BINDING_SINGLE(textureBindings, 2, RHIShaderResourceType::eSamplerWithTexture,
                                  m_pColorSampler, m_offscreenTextures.pAlbedo);
        ADD_SHADER_BINDING_SINGLE(textureBindings, 3, RHIShaderResourceType::eSamplerWithTexture,
                                  m_pColorSampler, m_offscreenTextures.pMetallicRoughness);
        ADD_SHADER_BINDING_SINGLE(textureBindings, 4, RHIShaderResourceType::eSamplerWithTexture,
                                  m_pColorSampler, m_offscreenTextures.pEmissiveOcclusion);
        ADD_SHADER_BINDING_SINGLE(textureBindings, 5, RHIShaderResourceType::eSamplerWithTexture,
                                  m_pDepthSampler, m_offscreenTextures.pDepth);
        ADD_SHADER_BINDING_SINGLE(textureBindings, 6, RHIShaderResourceType::eSamplerWithTexture,
                                  envTexture.pIrradianceSampler, envTexture.pIrradiance);
        ADD_SHADER_BINDING_SINGLE(textureBindings, 7, RHIShaderResourceType::eSamplerWithTexture,
                                  envTexture.pPrefilteredSampler, envTexture.pPrefiltered);
        ADD_SHADER_BINDING_SINGLE(textureBindings, 8, RHIShaderResourceType::eSamplerWithTexture,
                                  envTexture.pLutBRDFSampler, envTexture.pLutBRDF);

        GraphicsPassResourceUpdater updater(m_pRenderDevice, m_gfxPasses.pSceneLighting);
        updater.SetShaderResourceBinding(0, std::move(textureBindings))
            .SetShaderResourceBinding(1, std::move(bufferBindings))
            .Update();
    }
}
} // namespace zen::rc