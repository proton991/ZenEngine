#include "Graphics/RenderCore/V2/SceneRenderer.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"
#include "Systems/Camera.h"
#include "Systems/SceneEditor.h"
#include "AssetLib/GLTFLoader.h"
#include "Graphics/RenderCore/V2/SkyboxRenderer.h"

namespace zen::rc
{

SceneRenderer::SceneRenderer(RenderDevice* renderDevice, RHIViewport* viewport) :
    m_renderDevice(renderDevice), m_viewport(viewport)
{
    m_skyboxRenderer = new SkyboxRenderer(m_renderDevice, m_viewport);
    m_skyboxRenderer->Init();
}

void SceneRenderer::Init()
{
    PrepareTextures();

    BuildGraphicsPasses();

    UpdateGraphicsPassResources();
}

void SceneRenderer::Destroy()
{
    m_skyboxRenderer->Destroy();
}

void SceneRenderer::SetScene(const SceneData& sceneData)
{
    m_rebuildRDG = true;
    Clear();
    m_camera = sceneData.camera;
    m_scene  = sceneData.scene;

    std::memcpy(m_sceneUniformData.lightPositions, sceneData.lightPositions,
                sizeof(sceneData.lightPositions));
    std::memcpy(m_sceneUniformData.lightColors, sceneData.lightColors,
                sizeof(sceneData.lightColors));
    std::memcpy(m_sceneUniformData.lightIntensities, sceneData.lightIntensities,
                sizeof(sceneData.lightIntensities));

    sys::SceneEditor::CenterAndNormalizeScene(m_scene);

    for (auto* node : m_scene->GetRenderableNodes())
    {
        m_nodesData.emplace_back(node->GetData());
    }

    uint32_t vbSize = sceneData.numVertices * sizeof(asset::Vertex);
    m_vertexBuffer  = m_renderDevice->CreateVertexBuffer(
        vbSize, reinterpret_cast<const uint8_t*>(sceneData.vertices));

    m_indexBuffer =
        m_renderDevice->CreateIndexBuffer(sceneData.numIndices * sizeof(uint32_t),
                                          reinterpret_cast<const uint8_t*>(sceneData.indices));
    LoadSceneMaterials();

    LoadSceneTextures();

    PrepareBuffers();

    m_sceneLoaded = true;
}

void SceneRenderer::LoadSceneMaterials()
{
    auto sgMaterials = m_scene->GetComponents<sg::Material>();
    m_materialUniforms.reserve(sgMaterials.size());
    for (const auto* mat : sgMaterials)
    {
        m_materialUniforms.emplace_back(mat->data);
    }
}

void SceneRenderer::LoadSceneTextures()
{
    // default base color texture
    m_defaultBaseColorTexture = m_renderDevice->LoadTexture2D("wood.png");
    // scene textures
    m_renderDevice->LoadSceneTextures(m_scene, m_sceneTextures);
    // environment texture
    m_renderDevice->LoadTextureEnv("papermill.ktx", &m_envTexture, m_skyboxRenderer);
}

void SceneRenderer::DrawScene()
{
    m_renderDevice->UpdateBuffer(m_cameraUBO, sizeof(sys::CameraUniformData),
                                 m_camera->GetUniformData());
    m_renderDevice->UpdateBuffer(m_sceneUBO, sizeof(SceneUniformData),
                                 reinterpret_cast<const uint8_t*>(&m_sceneUniformData));

    m_skyboxRenderer->PrepareRenderWorkload(m_envTexture.skybox, m_cameraUBO);

    if (m_rebuildRDG)
    {
        BuildRenderGraph();
        m_rebuildRDG = false;
    }

    std::vector RDGs{
        m_skyboxRenderer->GetRenderGraph(), // 1st pass: skybox
        m_rdg.Get()                         // 2nd & 3rd pass: deferred scene
    };

    m_renderDevice->ExecuteFrame(m_viewport, RDGs);
}

void SceneRenderer::OnResize(uint32_t width, uint32_t height)
{
    m_rebuildRDG = true;
    m_renderDevice->ResizeViewport(m_viewport, width, height);
    // update graphics pass framebuffer
    m_gfxPasses.sceneLighting.framebuffer =
        m_viewport->GetCompatibleFramebufferForBackBuffer(m_gfxPasses.sceneLighting.renderPass);
    m_skyboxRenderer->OnResize();
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

void SceneRenderer::PrepareBuffers()
{
    {
        // camera ubo
        m_cameraUBO = m_renderDevice->CreateUniformBuffer(sizeof(sys::CameraUniformData),
                                                          m_camera->GetUniformData());
    }

    {
        // prepare node ssbo
        auto ssboSize = sizeof(sg::NodeData) * m_nodesData.size();
        m_nodeSSBO    = m_renderDevice->CreateStorageBuffer(
            ssboSize, reinterpret_cast<const uint8_t*>(m_nodesData.data()));
        m_renderDevice->UpdateBuffer(m_nodeSSBO, ssboSize,
                                     reinterpret_cast<const uint8_t*>(m_nodesData.data()));
    }

    {
        // prepare material ssbo
        const auto ssboSize = sizeof(sg::MaterialData) * m_materialUniforms.size();
        m_materialSSBO      = m_renderDevice->CreateStorageBuffer(
            ssboSize, reinterpret_cast<const uint8_t*>(m_materialUniforms.data()));
    }

    {
        // scene ubo
        m_sceneUniformData.viewPos = Vec4(m_camera->GetPos(), 1.0f);

        const auto uboSize = sizeof(SceneUniformData);
        m_sceneUBO         = m_renderDevice->CreateUniformBuffer(
            uboSize, reinterpret_cast<const uint8_t*>(&m_sceneUniformData));
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
            builder
                .SetPreloadedShaderName("DeferredPBR")
                // builder.SetVertexShader("SceneRenderer/offscreen.vert.spv")
                //     .SetFragmentShader("SceneRenderer/offscreen.frag.spv")
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
            builder.SetVertexShader("SceneRenderer/deferred.vert.spv")
                .SetFragmentShader("SceneRenderer/deferred.frag.spv")
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
    m_rdg->AddGraphicsPassBindVertexBufferNode(pass, m_vertexBuffer, {0});
    m_rdg->AddGraphicsPassBindIndexBufferNode(pass, m_indexBuffer, DataFormat::eR32UInt);
    m_rdg->AddGraphicsPassSetViewportNode(pass, viewport);
    m_rdg->AddGraphicsPassSetScissorNode(pass, area);
    for (auto* node : m_scene->GetRenderableNodes())
    {
        m_pushConstantsData.nodeIndex = node->GetRenderableIndex();
        for (auto* subMesh : node->GetComponent<sg::Mesh>()->GetSubMeshes())
        {
            m_pushConstantsData.materialIndex = subMesh->GetMaterial()->index;
            m_rdg->AddGraphicsPassSetPushConstants(pass, &m_pushConstantsData,
                                                   sizeof(PushConstantNode));
            m_rdg->AddGraphicsPassDrawIndexedNode(pass, subMesh->GetIndexCount(), 1,
                                                  subMesh->GetFirstIndex(), 0, 0);
        }
    }
}

void SceneRenderer::Clear()
{
    if (m_sceneLoaded)
    {
        // clear currently cached scene data
        m_renderDevice->DestroyBuffer(m_vertexBuffer);
        m_renderDevice->DestroyBuffer(m_indexBuffer);
        m_renderDevice->DestroyBuffer(m_nodeSSBO);
        m_renderDevice->DestroyBuffer(m_materialSSBO);

        m_pushConstantsData = {};
        m_nodesData.clear();
        m_materialUniforms.clear();
    }
}

void SceneRenderer::UpdateGraphicsPassResources()
{
    {
        std::vector<ShaderResourceBinding> bufferBindings;
        std::vector<ShaderResourceBinding> textureBindings;
        // buffers
        ADD_SHADER_BINDING_SINGLE(bufferBindings, 0, ShaderResourceType::eUniformBuffer,
                                  m_cameraUBO);
        ADD_SHADER_BINDING_SINGLE(bufferBindings, 1, ShaderResourceType::eStorageBuffer,
                                  m_nodeSSBO);
        ADD_SHADER_BINDING_SINGLE(bufferBindings, 2, ShaderResourceType::eStorageBuffer,
                                  m_materialSSBO);
        // texture array
        ADD_SHADER_BINDING_TEXTURE_ARRAY(textureBindings, 0,
                                         ShaderResourceType::eSamplerWithTexture, m_colorSampler,
                                         m_sceneTextures)

        rc::GraphicsPassResourceUpdater updater(m_renderDevice, &m_gfxPasses.offscreen);
        updater.SetShaderResourceBinding(0, bufferBindings)
            .SetShaderResourceBinding(1, textureBindings)
            .Update();
    }
    {
        std::vector<ShaderResourceBinding> bufferBindings;
        std::vector<ShaderResourceBinding> textureBindings;
        // buffer
        ADD_SHADER_BINDING_SINGLE(bufferBindings, 0, ShaderResourceType::eUniformBuffer,
                                  m_sceneUBO);

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
                                  m_envTexture.irradianceSampler, m_envTexture.irradiance);
        ADD_SHADER_BINDING_SINGLE(textureBindings, 7, ShaderResourceType::eSamplerWithTexture,
                                  m_envTexture.prefilteredSampler, m_envTexture.prefiltered);
        ADD_SHADER_BINDING_SINGLE(textureBindings, 8, ShaderResourceType::eSamplerWithTexture,
                                  m_envTexture.lutBRDFSampler, m_envTexture.lutBRDF);

        rc::GraphicsPassResourceUpdater updater(m_renderDevice, &m_gfxPasses.sceneLighting);
        updater.SetShaderResourceBinding(0, textureBindings)
            .SetShaderResourceBinding(1, bufferBindings)
            .Update();
    }
}
} // namespace zen::rc
