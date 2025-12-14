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
DeferredLightingRenderer::DeferredLightingRenderer(RenderDevice* renderDevice,
                                                   RHIViewport* viewport) :
    m_renderDevice(renderDevice), m_viewport(viewport)
{}

void DeferredLightingRenderer::Init()
{
    PrepareTextures();

    BuildGraphicsPasses();
}

void DeferredLightingRenderer::Destroy()
{
    m_renderDevice->DestroyTexture(m_offscreenTextures.position);
    m_renderDevice->DestroyTexture(m_offscreenTextures.normal);
    m_renderDevice->DestroyTexture(m_offscreenTextures.albedo);
    m_renderDevice->DestroyTexture(m_offscreenTextures.metallicRoughness);
    m_renderDevice->DestroyTexture(m_offscreenTextures.emissiveOcclusion);
    m_renderDevice->DestroyTexture(m_offscreenTextures.depth);
}

void DeferredLightingRenderer::PrepareRenderWorkload()
{
    if (m_rebuildRDG)
    {
        BuildRenderGraph();
        m_rebuildRDG = false;
    }
    m_gfxPasses.offscreen->shaderProgram->UpdateUniformBuffer("uCameraData",
                                                              m_scene->GetCameraUniformData(), 0);
    m_gfxPasses.sceneLighting->shaderProgram->UpdateUniformBuffer(
        "uSceneData", m_scene->GetSceneUniformData(), 0);
}

void DeferredLightingRenderer::OnResize()
{
    m_rebuildRDG = true;
    m_renderDevice->UpdateGraphicsPassOnResize(m_gfxPasses.sceneLighting, m_viewport);
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
        // m_offscreenTextures.position = m_renderDevice->CreateTexture(texInfo);

        TextureFormat texFormat{};
        texFormat.dimension   = TextureDimension::e2D;
        texFormat.format      = DataFormat::eR16G16B16A16SFloat;
        texFormat.width       = RenderConfig::GetInstance().offScreenFbSize;
        texFormat.height      = RenderConfig::GetInstance().offScreenFbSize;
        texFormat.depth       = 1;
        texFormat.arrayLayers = 1;
        texFormat.mipmaps     = 1;

        m_offscreenTextures.position =
            m_renderDevice->CreateTextureColorRT(texFormat, usageHint, "offscreen_position");
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
        // m_offscreenTextures.normal = m_renderDevice->CreateTexture(texInfo);

        TextureFormat texFormat{};
        texFormat.dimension   = TextureDimension::e2D;
        texFormat.format      = DataFormat::eR16G16B16A16SFloat;
        texFormat.width       = RenderConfig::GetInstance().offScreenFbSize;
        texFormat.height      = RenderConfig::GetInstance().offScreenFbSize;
        texFormat.depth       = 1;
        texFormat.arrayLayers = 1;
        texFormat.mipmaps     = 1;

        m_offscreenTextures.normal =
            m_renderDevice->CreateTextureColorRT(texFormat, usageHint, "offscreen_normal");
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
        // m_offscreenTextures.albedo = m_renderDevice->CreateTexture(texInfo);
        //
        // texInfo.name                          = "offscreen_roughness";
        // m_offscreenTextures.metallicRoughness = m_renderDevice->CreateTexture(texInfo);
        //
        // texInfo.name                          = "offscreen_emissive_occlusion";
        // m_offscreenTextures.emissiveOcclusion = m_renderDevice->CreateTexture(texInfo);

        TextureFormat texFormat{};
        texFormat.dimension   = TextureDimension::e2D;
        texFormat.format      = DataFormat::eR8G8B8A8UNORM;
        texFormat.width       = RenderConfig::GetInstance().offScreenFbSize;
        texFormat.height      = RenderConfig::GetInstance().offScreenFbSize;
        texFormat.depth       = 1;
        texFormat.arrayLayers = 1;
        texFormat.mipmaps     = 1;

        m_offscreenTextures.albedo =
            m_renderDevice->CreateTextureColorRT(texFormat, usageHint, "offscreen_albedo");

        m_offscreenTextures.metallicRoughness =
            m_renderDevice->CreateTextureColorRT(texFormat, usageHint, "offscreen_roughness");

        m_offscreenTextures.emissiveOcclusion = m_renderDevice->CreateTextureColorRT(
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
        // m_offscreenTextures.depth = m_renderDevice->CreateTexture(texInfo);

        TextureFormat texFormat{};
        texFormat.dimension   = TextureDimension::e2D;
        texFormat.format      = m_viewport->GetDepthStencilFormat();
        texFormat.width       = RenderConfig::GetInstance().offScreenFbSize;
        texFormat.height      = RenderConfig::GetInstance().offScreenFbSize;
        texFormat.depth       = 1;
        texFormat.arrayLayers = 1;
        texFormat.mipmaps     = 1;

        m_offscreenTextures.depth =
            m_renderDevice->CreateTextureDepthStencilRT(texFormat, usageHint, "offscreen_depth");
    }
    // offscreen color texture sampler
    {
        RHISamplerCreateInfo samplerInfo{};
        samplerInfo.borderColor = RHISamplerBorderColor::eFloatOpaqueWhite;
        m_depthSampler          = m_renderDevice->CreateSampler(samplerInfo);
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
        m_colorSampler          = m_renderDevice->CreateSampler(samplerInfo);
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
        pso.colorBlendState  = RHIGfxPipelineColorBlendState::CreateDisabled(5);
        pso.dynamicStates.push_back(RHIDynamicState::eScissor);
        pso.dynamicStates.push_back(RHIDynamicState::eViewPort);

        rc::GraphicsPassBuilder builder(m_renderDevice);
        m_gfxPasses.offscreen =
            builder
                .SetShaderProgramName("GBufferSP")
                // .SetNumSamples(SampleCount::e1)
                // (World space) Positions
                .AddColorRenderTarget(m_offscreenTextures.position)
                // (World space) Normals
                .AddColorRenderTarget(m_offscreenTextures.normal)
                // Albedo (color)
                .AddColorRenderTarget(m_offscreenTextures.albedo)
                // metallicRoughness
                .AddColorRenderTarget(m_offscreenTextures.metallicRoughness)
                // emissiveOcclusion
                .AddColorRenderTarget(m_offscreenTextures.emissiveOcclusion)
                .SetDepthStencilTarget(m_offscreenTextures.depth, RHIRenderTargetLoadOp::eClear,
                                       RHIRenderTargetStoreOp::eStore)
                .SetPipelineState(pso)
                .SetFramebufferInfo(m_viewport, RenderConfig::GetInstance().offScreenFbSize,
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
        pso.dynamicStates.push_back(RHIDynamicState::eScissor);
        pso.dynamicStates.push_back(RHIDynamicState::eViewPort);
        pso.colorBlendState   = RHIGfxPipelineColorBlendState::CreateDisabled(1);
        pso.depthStencilState = RHIGfxPipelineDepthStencilState::Create(
            true, true, RHIDepthCompareOperator::eLessOrEqual);

        rc::GraphicsPassBuilder builder(m_renderDevice);
        m_gfxPasses.sceneLighting =
            builder
                .SetShaderProgramName("DeferredLightingSP")
                // .SetNumSamples(SampleCount::e1)
                .AddViewportColorRT(m_viewport, RHIRenderTargetLoadOp::eLoad)
                .SetViewportDepthStencilRT(m_viewport, RHIRenderTargetLoadOp::eClear,
                                           RHIRenderTargetStoreOp::eStore)
                .SetPipelineState(pso)
                .SetFramebufferInfo(m_viewport)
                .SetTag("SceneLighting")
                .Build();
    }
}

void DeferredLightingRenderer::BuildRenderGraph()
{
    m_rdg = MakeUnique<RenderGraph>("deferred_lighting_rdg");
    m_rdg->Begin();
    // offscreen pass
    {
        const uint32_t cFbSize = RenderConfig::GetInstance().offScreenFbSize;
        std::vector<RHIRenderPassClearValue> clearValues(6);
        clearValues[0].color   = {0.0f, 0.0f, 0.0f, 0.0f};
        clearValues[1].color   = {0.0f, 0.0f, 0.0f, 0.0f};
        clearValues[2].color   = {0.0f, 0.0f, 0.0f, 0.0f};
        clearValues[3].color   = {0.0f, 0.0f, 0.0f, 0.0f};
        clearValues[4].color   = {0.0f, 0.0f, 0.0f, 0.0f};
        clearValues[5].depth   = 1.0f;
        clearValues[5].stencil = 0;

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

        auto* pass = m_rdg->AddGraphicsPassNode(m_gfxPasses.offscreen, area, clearValues,
                                                "offscreen_gbuffer");
        // m_rdg->DeclareTextureAccessForPass(
        //     pass, m_offscreenTextures.position, RHITextureUsage::eColorAttachment,
        //     RHITextureSubResourceRange::Color(), RHIAccessMode::eReadWrite);
        // m_rdg->DeclareTextureAccessForPass(
        //     pass, m_offscreenTextures.normal, RHITextureUsage::eColorAttachment,
        //     RHITextureSubResourceRange::Color(), RHIAccessMode::eReadWrite);
        // m_rdg->DeclareTextureAccessForPass(
        //     pass, m_offscreenTextures.albedo, RHITextureUsage::eColorAttachment,
        //     RHITextureSubResourceRange::Color(), RHIAccessMode::eReadWrite);
        // m_rdg->DeclareTextureAccessForPass(
        //     pass, m_offscreenTextures.metallicRoughness, RHITextureUsage::eColorAttachment,
        //     RHITextureSubResourceRange::Color(), RHIAccessMode::eReadWrite);
        // m_rdg->DeclareTextureAccessForPass(
        //     pass, m_offscreenTextures.emissiveOcclusion, RHITextureUsage::eColorAttachment,
        //     RHITextureSubResourceRange::Color(), RHIAccessMode::eReadWrite);
        // m_rdg->DeclareTextureAccessForPass(
        //     pass, m_offscreenTextures.depth, RHITextureUsage::eDepthStencilAttachment,
        //     RHITextureSubResourceRange::DepthStencil(), RHIAccessMode::eReadWrite);
        AddMeshDrawNodes(pass, area, vp);
    }
    // scene lighting pass
    {
        std::vector<RHIRenderPassClearValue> clearValues(2);
        clearValues[0].color   = {0.8f, 0.8f, 0.8f, 1.0f};
        clearValues[1].depth   = 1.0f;
        clearValues[1].stencil = 0;

        Rect2i area;
        area.minX = 0;
        area.minY = 0;
        area.maxX = static_cast<int>(m_viewport->GetWidth());
        area.maxY = static_cast<int>(m_viewport->GetHeight());

        Rect2<float> vp;
        vp.minX = 0.0f;
        vp.minY = 0.0f;
        vp.maxX = static_cast<float>(m_viewport->GetWidth());
        vp.maxY = static_cast<float>(m_viewport->GetHeight());

        auto* pass = m_rdg->AddGraphicsPassNode(m_gfxPasses.sceneLighting, area, clearValues,
                                                "deferred_lighting");
        // m_rdg->DeclareTextureAccessForPass(pass, m_offscreenTextures.position,
        //                                    RHITextureUsage::eSampled, RHITextureSubResourceRange::Color(),
        //                                    RHIAccessMode::eRead);
        // m_rdg->DeclareTextureAccessForPass(pass, m_offscreenTextures.normal, RHITextureUsage::eSampled,
        //                                    RHITextureSubResourceRange::Color(),
        //                                    RHIAccessMode::eRead);
        // m_rdg->DeclareTextureAccessForPass(pass, m_offscreenTextures.albedo, RHITextureUsage::eSampled,
        //                                    RHITextureSubResourceRange::Color(),
        //                                    RHIAccessMode::eRead);
        // m_rdg->DeclareTextureAccessForPass(pass, m_offscreenTextures.metallicRoughness,
        //                                    RHITextureUsage::eSampled, RHITextureSubResourceRange::Color(),
        //                                    RHIAccessMode::eRead);
        // m_rdg->DeclareTextureAccessForPass(pass, m_offscreenTextures.emissiveOcclusion,
        //                                    RHITextureUsage::eSampled, RHITextureSubResourceRange::Color(),
        //                                    RHIAccessMode::eRead);
        // m_rdg->DeclareTextureAccessForPass(pass, m_offscreenTextures.depth, RHITextureUsage::eSampled,
        //                                    RHITextureSubResourceRange::DepthStencil(),
        //                                    RHIAccessMode::eRead);
        // Final composition
        // This is done by simply drawing a full screen quad
        // The fragment shader then combines the deferred attachments into the final image
        m_rdg->AddGraphicsPassSetScissorNode(pass, area);
        m_rdg->AddGraphicsPassSetViewportNode(pass, vp);
        m_rdg->AddGraphicsPassDrawNode(pass, 3, 1);
    }
    m_rdg->End();
}

void DeferredLightingRenderer::AddMeshDrawNodes(RDGPassNode* pass,
                                                const Rect2<int>& area,
                                                const Rect2<float>& viewport)
{
    GBufferSP* shaderProgram = dynamic_cast<GBufferSP*>(m_gfxPasses.offscreen->shaderProgram);
    m_rdg->AddGraphicsPassBindVertexBufferNode(pass, m_scene->GetVertexBuffer(), {0});
    m_rdg->AddGraphicsPassBindIndexBufferNode(pass, m_scene->GetIndexBuffer(),
                                              DataFormat::eR32UInt);
    m_rdg->AddGraphicsPassSetViewportNode(pass, viewport);
    m_rdg->AddGraphicsPassSetScissorNode(pass, area);
    for (auto* node : m_scene->GetRenderableNodes())
    {
        shaderProgram->pushConstantsData.nodeIndex = node->GetRenderableIndex();
        for (auto* subMesh : node->GetComponent<sg::Mesh>()->GetSubMeshes())
        {
            shaderProgram->pushConstantsData.materialIndex = subMesh->GetMaterial()->index;
            m_rdg->AddGraphicsPassSetPushConstants(pass, &shaderProgram->pushConstantsData,
                                                   sizeof(GBufferSP::PushConstantsData));
            m_rdg->AddGraphicsPassDrawIndexedNode(pass, subMesh->GetIndexCount(), 1,
                                                  subMesh->GetFirstIndex(), 0, 0);
        }
    }
}

void DeferredLightingRenderer::UpdateGraphicsPassResources()
{
    const EnvTexture& envTexture = m_scene->GetEnvTexture();
    {
        std::vector<RHIShaderResourceBinding> bufferBindings;
        std::vector<RHIShaderResourceBinding> textureBindings;
        // buffers
        ADD_SHADER_BINDING_SINGLE(
            bufferBindings, 0, RHIShaderResourceType::eUniformBuffer,
            m_gfxPasses.offscreen->shaderProgram->GetUniformBufferHandle("uCameraData"));
        ADD_SHADER_BINDING_SINGLE(bufferBindings, 1, RHIShaderResourceType::eStorageBuffer,
                                  m_scene->GetNodesDataSSBO());
        ADD_SHADER_BINDING_SINGLE(bufferBindings, 2, RHIShaderResourceType::eStorageBuffer,
                                  m_scene->GetMaterialsDataSSBO());
        // texture array
        ADD_SHADER_BINDING_TEXTURE_ARRAY(textureBindings, 0,
                                         RHIShaderResourceType::eSamplerWithTexture, m_colorSampler,
                                         m_scene->GetSceneTextures())

        GraphicsPassResourceUpdater updater(m_renderDevice, m_gfxPasses.offscreen);
        updater.SetShaderResourceBinding(0, bufferBindings)
            .SetShaderResourceBinding(1, textureBindings)
            .Update();
    }
    {
        std::vector<RHIShaderResourceBinding> bufferBindings;
        std::vector<RHIShaderResourceBinding> textureBindings;
        // buffer
        ADD_SHADER_BINDING_SINGLE(
            bufferBindings, 0, RHIShaderResourceType::eUniformBuffer,
            m_gfxPasses.sceneLighting->shaderProgram->GetUniformBufferHandle("uSceneData"));
        // textures
        ADD_SHADER_BINDING_SINGLE(textureBindings, 0, RHIShaderResourceType::eSamplerWithTexture,
                                  m_colorSampler, m_offscreenTextures.position);
        ADD_SHADER_BINDING_SINGLE(textureBindings, 1, RHIShaderResourceType::eSamplerWithTexture,
                                  m_colorSampler, m_offscreenTextures.normal);
        ADD_SHADER_BINDING_SINGLE(textureBindings, 2, RHIShaderResourceType::eSamplerWithTexture,
                                  m_colorSampler, m_offscreenTextures.albedo);
        ADD_SHADER_BINDING_SINGLE(textureBindings, 3, RHIShaderResourceType::eSamplerWithTexture,
                                  m_colorSampler, m_offscreenTextures.metallicRoughness);
        ADD_SHADER_BINDING_SINGLE(textureBindings, 4, RHIShaderResourceType::eSamplerWithTexture,
                                  m_colorSampler, m_offscreenTextures.emissiveOcclusion);
        ADD_SHADER_BINDING_SINGLE(textureBindings, 5, RHIShaderResourceType::eSamplerWithTexture,
                                  m_depthSampler, m_offscreenTextures.depth);
        ADD_SHADER_BINDING_SINGLE(textureBindings, 6, RHIShaderResourceType::eSamplerWithTexture,
                                  envTexture.irradianceSampler, envTexture.irradiance);
        ADD_SHADER_BINDING_SINGLE(textureBindings, 7, RHIShaderResourceType::eSamplerWithTexture,
                                  envTexture.prefilteredSampler, envTexture.prefiltered);
        ADD_SHADER_BINDING_SINGLE(textureBindings, 8, RHIShaderResourceType::eSamplerWithTexture,
                                  envTexture.lutBRDFSampler, envTexture.lutBRDF);

        GraphicsPassResourceUpdater updater(m_renderDevice, m_gfxPasses.sceneLighting);
        updater.SetShaderResourceBinding(0, textureBindings)
            .SetShaderResourceBinding(1, bufferBindings)
            .Update();
    }
}
} // namespace zen::rc