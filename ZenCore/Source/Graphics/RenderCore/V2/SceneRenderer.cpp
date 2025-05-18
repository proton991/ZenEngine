#include "Graphics/RenderCore/V2/Renderer/SceneRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/SkyboxRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/RendererServer.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RenderCore/V2/Renderer/ShadowMapRenderer.h"
#include "SceneGraph/Camera.h"

using namespace zen::rhi;

namespace zen::rc
{
SceneRenderer::SceneRenderer(RenderDevice* renderDevice, rhi::RHIViewport* viewport) :
    m_renderDevice(renderDevice), m_viewport(viewport)
{}

void SceneRenderer::Init()
{
    PrepareTextures();

    BuildGraphicsPasses();
}

void SceneRenderer::Destroy() {}

void SceneRenderer::DrawScene()
{
    m_scene->Update();

    SkyboxRenderer* skyboxRenderer = m_renderDevice->GetRendererServer()->RequestSkyboxRenderer();
    skyboxRenderer->PrepareRenderWorkload(
        m_scene->GetEnvTexture().skybox,
        m_gfxPasses.offscreen.shaderProgram->GetUniformBufferHandle("uCameraData"));

    m_gfxPasses.offscreen.shaderProgram->UpdateUniformBuffer("uCameraData",
                                                             m_scene->GetCameraUniformData(), 0);

    m_gfxPasses.sceneLighting.shaderProgram->UpdateUniformBuffer("uSceneData",
                                                                 m_scene->GetSceneUniformData(), 0);

    if (m_rebuildRDG)
    {
        BuildRenderGraph();
        m_rebuildRDG = false;
    }

    ShadowMapRenderer* shadowMapRenderer =
        m_renderDevice->GetRendererServer()->RequestShadowMapRenderer();
    shadowMapRenderer->PrepareRenderWorkload();

    std::vector<RenderGraph*> RDGs;
    if (m_renderDevice->SupportVoxelizer())
    {
        VoxelRenderer* voxelRenderer = m_renderDevice->GetRendererServer()->RequestVoxelRenderer();
        voxelRenderer->PrepareRenderWorkload();
        RDGs.push_back(shadowMapRenderer->GetRenderGraph()); // shadowMap
        RDGs.push_back(voxelRenderer->GetRenderGraph());     // voxel
        // RDGs.push_back(skyboxRenderer->GetRenderGraph()); // skybox
        // RDGs.push_back(m_rdg.Get());                      // deferred pbr
    }
    else
    {
        RDGs.push_back(skyboxRenderer->GetRenderGraph()); // skybox
        RDGs.push_back(m_rdg.Get());                      // deferred pbr
    }

    m_renderDevice->ExecuteFrame(m_viewport, RDGs);
}

void SceneRenderer::OnResize(uint32_t width, uint32_t height)
{
    m_rebuildRDG = true;
    m_renderDevice->ResizeViewport(m_viewport, width, height);
    // update graphics pass framebuffer
    m_gfxPasses.sceneLighting.framebuffer =
        m_viewport->GetCompatibleFramebufferForBackBuffer(m_gfxPasses.sceneLighting.renderPass);
    m_renderDevice->GetRendererServer()->RequestSkyboxRenderer()->OnResize();
}

void SceneRenderer::PrepareTextures()
{
    // (World space) Positions
    {
        rhi::TextureInfo texInfo{};
        texInfo.type        = rhi::TextureType::e2D;
        texInfo.format      = DataFormat::eR16G16B16A16SFloat;
        texInfo.width       = RenderConfig::GetInstance().offScreenFbSize;
        texInfo.height      = RenderConfig::GetInstance().offScreenFbSize;
        texInfo.depth       = 1;
        texInfo.mipmaps     = 1;
        texInfo.arrayLayers = 1;
        texInfo.samples     = SampleCount::e1;
        texInfo.usageFlags.SetFlag(TextureUsageFlagBits::eColorAttachment);
        texInfo.usageFlags.SetFlag(TextureUsageFlagBits::eSampled);
        m_offscreenTextures.position = m_renderDevice->CreateTexture(texInfo, "offscreen_position");
    }
    // (World space) Normals
    {
        rhi::TextureInfo texInfo{};
        texInfo.type        = rhi::TextureType::e2D;
        texInfo.format      = DataFormat::eR16G16B16A16SFloat;
        texInfo.width       = RenderConfig::GetInstance().offScreenFbSize;
        texInfo.height      = RenderConfig::GetInstance().offScreenFbSize;
        texInfo.depth       = 1;
        texInfo.mipmaps     = 1;
        texInfo.arrayLayers = 1;
        texInfo.samples     = SampleCount::e1;
        texInfo.usageFlags.SetFlag(TextureUsageFlagBits::eColorAttachment);
        texInfo.usageFlags.SetFlag(TextureUsageFlagBits::eSampled);
        m_offscreenTextures.normal = m_renderDevice->CreateTexture(texInfo, "offscreen_normal");
    }
    // color
    {
        rhi::TextureInfo texInfo{};
        texInfo.type        = rhi::TextureType::e2D;
        texInfo.format      = DataFormat::eR8G8B8A8UNORM;
        texInfo.width       = RenderConfig::GetInstance().offScreenFbSize;
        texInfo.height      = RenderConfig::GetInstance().offScreenFbSize;
        texInfo.depth       = 1;
        texInfo.mipmaps     = 1;
        texInfo.arrayLayers = 1;
        texInfo.samples     = SampleCount::e1;
        texInfo.usageFlags.SetFlag(TextureUsageFlagBits::eColorAttachment);
        texInfo.usageFlags.SetFlag(TextureUsageFlagBits::eSampled);
        m_offscreenTextures.albedo = m_renderDevice->CreateTexture(texInfo, "offscreen_albedo");
        m_offscreenTextures.metallicRoughness =
            m_renderDevice->CreateTexture(texInfo, "offscreen_roughness");
        m_offscreenTextures.emissiveOcclusion =
            m_renderDevice->CreateTexture(texInfo, "offscreen_emissive_occlusion");
    }
    // depth
    {

        rhi::TextureInfo texInfo{};
        texInfo.type        = rhi::TextureType::e2D;
        texInfo.format      = m_viewport->GetDepthStencilFormat();
        texInfo.type        = rhi::TextureType::e2D;
        texInfo.width       = RenderConfig::GetInstance().offScreenFbSize;
        texInfo.height      = RenderConfig::GetInstance().offScreenFbSize;
        texInfo.depth       = 1;
        texInfo.arrayLayers = 1;
        texInfo.mipmaps     = 1;
        texInfo.usageFlags.SetFlag(rhi::TextureUsageFlagBits::eDepthStencilAttachment);
        texInfo.usageFlags.SetFlag(rhi::TextureUsageFlagBits::eSampled);
        m_offscreenTextures.depth = m_renderDevice->CreateTexture(texInfo, "offscreen_depth");
    }
    // offscreen color texture sampler
    {
        SamplerInfo samplerInfo{};
        samplerInfo.borderColor = SamplerBorderColor::eFloatOpaqueWhite;
        m_depthSampler          = m_renderDevice->CreateSampler(samplerInfo);
    }
    // offscreen depth texture sampler
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

void SceneRenderer::BuildGraphicsPasses()
{
    // offscreen
    {
        GfxPipelineStates pso{};
        pso.rasterizationState          = {};
        pso.rasterizationState.cullMode = PolygonCullMode::eBack;

        pso.depthStencilState =
            GfxPipelineDepthStencilState::Create(true, true, CompareOperator::eLess);
        pso.multiSampleState = {};
        pso.colorBlendState  = GfxPipelineColorBlendState::CreateDisabled(5);
        pso.dynamicStates.push_back(DynamicState::eScissor);
        pso.dynamicStates.push_back(DynamicState::eViewPort);

        rc::GraphicsPassBuilder builder(m_renderDevice);
        m_gfxPasses.offscreen =
            builder.SetShaderProgramName("GBufferSP")
                .SetNumSamples(SampleCount::e1)
                // (World space) Positions
                .AddColorRenderTarget(DataFormat::eR16G16B16A16SFloat,
                                      TextureUsage::eColorAttachment, m_offscreenTextures.position)
                // (World space) Normals
                .AddColorRenderTarget(DataFormat::eR16G16B16A16SFloat,
                                      TextureUsage::eColorAttachment, m_offscreenTextures.normal)
                // Albedo (color)
                .AddColorRenderTarget(DataFormat::eR8G8B8A8UNORM, TextureUsage::eColorAttachment,
                                      m_offscreenTextures.albedo)
                // metallicRoughness
                .AddColorRenderTarget(DataFormat::eR8G8B8A8UNORM, TextureUsage::eColorAttachment,
                                      m_offscreenTextures.metallicRoughness)
                // emissiveOcclusion
                .AddColorRenderTarget(DataFormat::eR8G8B8A8UNORM, TextureUsage::eColorAttachment,
                                      m_offscreenTextures.emissiveOcclusion)
                .SetDepthStencilTarget(m_viewport->GetDepthStencilFormat(),
                                       m_offscreenTextures.depth, rhi::RenderTargetLoadOp::eClear,
                                       rhi::RenderTargetStoreOp::eStore)
                .SetPipelineState(pso)
                .SetFramebufferInfo(m_viewport, RenderConfig::GetInstance().offScreenFbSize,
                                    RenderConfig::GetInstance().offScreenFbSize)
                .SetTag("OffScreen")
                .Build();
    }

    // scene lighting
    {
        GfxPipelineStates pso{};
        pso.primitiveType      = DrawPrimitiveType::eTriangleList;
        pso.rasterizationState = {};
        pso.multiSampleState   = {};
        pso.dynamicStates.push_back(DynamicState::eScissor);
        pso.dynamicStates.push_back(DynamicState::eViewPort);
        pso.colorBlendState = GfxPipelineColorBlendState::CreateDisabled(1);
        pso.depthStencilState =
            GfxPipelineDepthStencilState::Create(true, true, CompareOperator::eLessOrEqual);

        rc::GraphicsPassBuilder builder(m_renderDevice);
        m_gfxPasses.sceneLighting =
            builder.SetShaderProgramName("DeferredLightingSP")
                .SetNumSamples(SampleCount::e1)
                .AddColorRenderTarget(m_viewport->GetSwapchainFormat(),
                                      TextureUsage::eColorAttachment,
                                      m_viewport->GetColorBackBuffer(), false)
                .SetDepthStencilTarget(m_viewport->GetDepthStencilFormat(),
                                       m_viewport->GetDepthStencilBackBuffer(),
                                       RenderTargetLoadOp::eClear, RenderTargetStoreOp::eStore)
                .SetPipelineState(pso)
                .SetFramebufferInfo(m_viewport)
                .SetTag("SceneLighting")
                .Build();
    }
}

void SceneRenderer::BuildRenderGraph()
{
    m_rdg = MakeUnique<RenderGraph>();
    m_rdg->Begin();
    // offscreen pass
    {
        const uint32_t cFbSize = RenderConfig::GetInstance().offScreenFbSize;
        std::vector<RenderPassClearValue> clearValues(6);
        clearValues[0].color   = {0.0f, 0.0f, 0.0f, 0.0f};
        clearValues[1].color   = {0.0f, 0.0f, 0.0f, 0.0f};
        clearValues[2].color   = {0.0f, 0.0f, 0.0f, 0.0f};
        clearValues[3].color   = {0.0f, 0.0f, 0.0f, 0.0f};
        clearValues[4].color   = {0.0f, 0.0f, 0.0f, 0.0f};
        clearValues[5].depth   = 1.0f;
        clearValues[5].stencil = 0;

        Rect2 area;
        area.minX = 0;
        area.minY = 0;
        area.maxX = static_cast<int>(cFbSize);
        area.maxY = static_cast<int>(cFbSize);

        Rect2<float> vp;
        vp.minX = 0.0f;
        vp.minY = 0.0f;
        vp.maxX = static_cast<float>(cFbSize);
        vp.maxY = static_cast<float>(cFbSize);

        auto* pass = m_rdg->AddGraphicsPassNode(m_gfxPasses.offscreen, area, clearValues, true);
        m_rdg->DeclareTextureAccessForPass(
            pass, m_offscreenTextures.position, TextureUsage::eColorAttachment,
            TextureSubResourceRange::Color(), rc::RDGAccessType::eReadWrite);
        m_rdg->DeclareTextureAccessForPass(
            pass, m_offscreenTextures.normal, TextureUsage::eColorAttachment,
            TextureSubResourceRange::Color(), rc::RDGAccessType::eReadWrite);
        m_rdg->DeclareTextureAccessForPass(
            pass, m_offscreenTextures.albedo, TextureUsage::eColorAttachment,
            TextureSubResourceRange::Color(), rc::RDGAccessType::eReadWrite);
        m_rdg->DeclareTextureAccessForPass(
            pass, m_offscreenTextures.metallicRoughness, TextureUsage::eColorAttachment,
            TextureSubResourceRange::Color(), rc::RDGAccessType::eReadWrite);
        m_rdg->DeclareTextureAccessForPass(
            pass, m_offscreenTextures.emissiveOcclusion, TextureUsage::eColorAttachment,
            TextureSubResourceRange::Color(), rc::RDGAccessType::eReadWrite);
        m_rdg->DeclareTextureAccessForPass(
            pass, m_offscreenTextures.depth, TextureUsage::eDepthStencilAttachment,
            TextureSubResourceRange::DepthStencil(), rc::RDGAccessType::eReadWrite);
        AddMeshDrawNodes(pass, area, vp);
    }
    // scene lighting pass
    {
        std::vector<RenderPassClearValue> clearValues(2);
        clearValues[0].color   = {0.8f, 0.8f, 0.8f, 1.0f};
        clearValues[1].depth   = 1.0f;
        clearValues[1].stencil = 0;

        Rect2 area;
        area.minX = 0;
        area.minY = 0;
        area.maxX = static_cast<int>(m_viewport->GetWidth());
        area.maxY = static_cast<int>(m_viewport->GetHeight());

        Rect2<float> vp;
        vp.minX = 0.0f;
        vp.minY = 0.0f;
        vp.maxX = static_cast<float>(m_viewport->GetWidth());
        vp.maxY = static_cast<float>(m_viewport->GetHeight());

        DynamicRHI* RHI = m_renderDevice->GetRHI();

        auto* pass = m_rdg->AddGraphicsPassNode(m_gfxPasses.sceneLighting, area, clearValues, true);
        m_rdg->DeclareTextureAccessForPass(pass, m_offscreenTextures.position,
                                           TextureUsage::eSampled, TextureSubResourceRange::Color(),
                                           rc::RDGAccessType::eRead);
        m_rdg->DeclareTextureAccessForPass(pass, m_offscreenTextures.normal, TextureUsage::eSampled,
                                           TextureSubResourceRange::Color(),
                                           rc::RDGAccessType::eRead);
        m_rdg->DeclareTextureAccessForPass(pass, m_offscreenTextures.albedo, TextureUsage::eSampled,
                                           TextureSubResourceRange::Color(),
                                           rc::RDGAccessType::eRead);
        m_rdg->DeclareTextureAccessForPass(pass, m_offscreenTextures.metallicRoughness,
                                           TextureUsage::eSampled, TextureSubResourceRange::Color(),
                                           rc::RDGAccessType::eRead);
        m_rdg->DeclareTextureAccessForPass(pass, m_offscreenTextures.emissiveOcclusion,
                                           TextureUsage::eSampled, TextureSubResourceRange::Color(),
                                           rc::RDGAccessType::eRead);
        m_rdg->DeclareTextureAccessForPass(pass, m_offscreenTextures.depth, TextureUsage::eSampled,
                                           TextureSubResourceRange::DepthStencil(),
                                           rc::RDGAccessType::eRead);
        // Final composition
        // This is done by simply drawing a full screen quad
        // The fragment shader then combines the deferred attachments into the final image
        m_rdg->AddGraphicsPassSetScissorNode(pass, area);
        m_rdg->AddGraphicsPassSetViewportNode(pass, vp);
        m_rdg->AddGraphicsPassDrawNode(pass, 3, 1);
    }
    m_rdg->End();
}

void SceneRenderer::AddMeshDrawNodes(RDGPassNode* pass,
                                     const Rect2<int>& area,
                                     const Rect2<float>& viewport)
{
    GBufferShaderProgram* shaderProgram =
        dynamic_cast<GBufferShaderProgram*>(m_gfxPasses.offscreen.shaderProgram);
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
                                                   sizeof(GBufferShaderProgram::PushConstantData));
            m_rdg->AddGraphicsPassDrawIndexedNode(pass, subMesh->GetIndexCount(), 1,
                                                  subMesh->GetFirstIndex(), 0, 0);
        }
    }
}

void SceneRenderer::UpdateGraphicsPassResources()
{
    const EnvTexture& envTexture = m_scene->GetEnvTexture();
    {
        std::vector<ShaderResourceBinding> bufferBindings;
        std::vector<ShaderResourceBinding> textureBindings;
        // buffers
        ADD_SHADER_BINDING_SINGLE(
            bufferBindings, 0, ShaderResourceType::eUniformBuffer,
            m_gfxPasses.offscreen.shaderProgram->GetUniformBufferHandle("uCameraData"));
        ADD_SHADER_BINDING_SINGLE(bufferBindings, 1, ShaderResourceType::eStorageBuffer,
                                  m_scene->GetNodesDataSSBO());
        ADD_SHADER_BINDING_SINGLE(bufferBindings, 2, ShaderResourceType::eStorageBuffer,
                                  m_scene->GetMaterialsDataSSBO());
        // texture array
        ADD_SHADER_BINDING_TEXTURE_ARRAY(textureBindings, 0,
                                         ShaderResourceType::eSamplerWithTexture, m_colorSampler,
                                         m_scene->GetSceneTextures())

        rc::GraphicsPassResourceUpdater updater(m_renderDevice, &m_gfxPasses.offscreen);
        updater.SetShaderResourceBinding(0, bufferBindings)
            .SetShaderResourceBinding(1, textureBindings)
            .Update();
    }
    {
        std::vector<ShaderResourceBinding> bufferBindings;
        std::vector<ShaderResourceBinding> textureBindings;
        // buffer
        ADD_SHADER_BINDING_SINGLE(
            bufferBindings, 0, ShaderResourceType::eUniformBuffer,
            m_gfxPasses.sceneLighting.shaderProgram->GetUniformBufferHandle("uSceneData"));
        // textures
        ADD_SHADER_BINDING_SINGLE(textureBindings, 0, ShaderResourceType::eSamplerWithTexture,
                                  m_colorSampler, m_offscreenTextures.position);
        ADD_SHADER_BINDING_SINGLE(textureBindings, 1, ShaderResourceType::eSamplerWithTexture,
                                  m_colorSampler, m_offscreenTextures.normal);
        ADD_SHADER_BINDING_SINGLE(textureBindings, 2, ShaderResourceType::eSamplerWithTexture,
                                  m_colorSampler, m_offscreenTextures.albedo);
        ADD_SHADER_BINDING_SINGLE(textureBindings, 3, ShaderResourceType::eSamplerWithTexture,
                                  m_colorSampler, m_offscreenTextures.metallicRoughness);
        ADD_SHADER_BINDING_SINGLE(textureBindings, 4, ShaderResourceType::eSamplerWithTexture,
                                  m_colorSampler, m_offscreenTextures.emissiveOcclusion);
        ADD_SHADER_BINDING_SINGLE(textureBindings, 5, ShaderResourceType::eSamplerWithTexture,
                                  m_depthSampler, m_offscreenTextures.depth);
        ADD_SHADER_BINDING_SINGLE(textureBindings, 6, ShaderResourceType::eSamplerWithTexture,
                                  envTexture.irradianceSampler, envTexture.irradiance);
        ADD_SHADER_BINDING_SINGLE(textureBindings, 7, ShaderResourceType::eSamplerWithTexture,
                                  envTexture.prefilteredSampler, envTexture.prefiltered);
        ADD_SHADER_BINDING_SINGLE(textureBindings, 8, ShaderResourceType::eSamplerWithTexture,
                                  envTexture.lutBRDFSampler, envTexture.lutBRDF);

        rc::GraphicsPassResourceUpdater updater(m_renderDevice, &m_gfxPasses.sceneLighting);
        updater.SetShaderResourceBinding(0, textureBindings)
            .SetShaderResourceBinding(1, bufferBindings)
            .Update();
    }
}
} // namespace zen::rc