#include "ShadowMapping.h"
#include "AssetLib/FastGLTFLoader.h"


ShadowMappingApp::ShadowMappingApp(const platform::WindowConfig& windowConfig) :
    Application(windowConfig, sg::CameraType::eFirstPerson)
{
    m_camera->SetPosition({1.0f, 1.0f, -2.0f});
}

void ShadowMappingApp::Prepare()
{
    Application::Prepare();
    LoadResources();
    PrepareOffscreen();
    BuildGraphicsPasses();
    BuildRenderGraph();
}

void ShadowMappingApp::Run()
{
    while (!m_window->ShouldClose())
    {
        auto frameTime = static_cast<float>(m_timer->Tick());
        m_animationTimer += frameTime * m_animationSpeed;
        if (m_animationTimer > 1.0f)
        {
            m_animationTimer -= 1.0f;
        }
        UpdateUniformBufferData();
        m_window->Update();
        m_camera->Update(frameTime);
        m_renderDevice->UpdateBuffer(m_offscreenUBO, sizeof(OffscreenUniformData),
                                     reinterpret_cast<const uint8_t*>(&m_offscreenUniformData));
        m_renderDevice->UpdateBuffer(m_sceneUBO, sizeof(SceneUniformData),
                                     reinterpret_cast<const uint8_t*>(&m_sceneUniformData));

        m_renderDevice->ExecuteFrame(m_viewport, m_rdg.Get());

        m_renderDevice->NextFrame();
    }
}

void ShadowMappingApp::Destroy()
{
    Application::Destroy();
}

void ShadowMappingApp::PrepareOffscreen()
{
    // offscreen depth texture
    rhi::TextureInfo textureInfo{};
    textureInfo.format      = cOffscreenDepthFormat;
    textureInfo.type        = rhi::TextureType::e2D;
    textureInfo.width       = cShadowMapSize;
    textureInfo.height      = cShadowMapSize;
    textureInfo.depth       = 1;
    textureInfo.arrayLayers = 1;
    textureInfo.mipmaps     = 1;
    textureInfo.usageFlags.SetFlag(rhi::TextureUsageFlagBits::eDepthStencilAttachment);
    textureInfo.usageFlags.SetFlag(rhi::TextureUsageFlagBits::eSampled);
    m_offscreenDepthTexture = m_renderDevice->CreateTexture(textureInfo, "offscreen_depth");
    // offscreen depth texture sampler
    SamplerInfo samplerInfo{};
    samplerInfo.borderColor = SamplerBorderColor::eFloatOpaqueWhite;
    m_depthSampler          = m_renderDevice->CreateSampler(samplerInfo);
}

void ShadowMappingApp::BuildGraphicsPasses()
{
    GfxPipelineStates pso{};
    pso.primitiveType      = DrawPrimitiveType::eTriangleList;
    pso.rasterizationState = {};
    //    pso.colorBlendState    = GfxPipelineColorBlendState::CreateDisabled();
    pso.depthStencilState =
        GfxPipelineDepthStencilState::Create(true, true, CompareOperator::eLessOrEqual);
    pso.depthStencilState.frontOp.fail      = rhi::StencilOperation::eKeep;
    pso.depthStencilState.frontOp.pass      = rhi::StencilOperation::eKeep;
    pso.depthStencilState.frontOp.depthFail = rhi::StencilOperation::eKeep;
    pso.depthStencilState.frontOp.compare   = rhi::CompareOperator::eNever;
    pso.depthStencilState.backOp.fail       = rhi::StencilOperation::eKeep;
    pso.depthStencilState.backOp.pass       = rhi::StencilOperation::eKeep;
    pso.depthStencilState.backOp.depthFail  = rhi::StencilOperation::eKeep;
    pso.multiSampleState                    = {};

    // offscreen
    {

        pso.dynamicStates.push_back(DynamicState::eScissor);
        pso.dynamicStates.push_back(DynamicState::eViewPort);
        pso.dynamicStates.push_back(DynamicState::eDepthBias);
        std::vector<ShaderResourceBinding> uboBindings;
        {
            ShaderResourceBinding binding0{};
            binding0.binding = 0;
            binding0.type    = ShaderResourceType::eUniformBuffer;
            binding0.handles.push_back(m_offscreenUBO);
            uboBindings.emplace_back(std::move(binding0));
        }
        rc::GraphicsPassBuilder builder(m_renderDevice);
        m_gfxPasses.offscreen =
            builder
                .AddShaderStage(rhi::ShaderStage::eVertex, "ShadowMapping/offscreen.vert.spv")
                //                .SetFragmentShader("ShadowMapping/offscreen.frag.spv")
                .SetNumSamples(SampleCount::e1)
                .SetDepthStencilTarget(cOffscreenDepthFormat, m_offscreenDepthTexture,
                                       rhi::RenderTargetLoadOp::eClear,
                                       rhi::RenderTargetStoreOp::eStore)
                .SetShaderResourceBinding(0, uboBindings)
                .SetPipelineState(pso)
                .SetFramebufferInfo(m_viewport, cShadowMapSize, cShadowMapSize)
                .SetTag("Offscreen")
                .Build();
    }

    // scene shadow
    {
        pso.dynamicStates.clear();
        pso.dynamicStates.push_back(DynamicState::eScissor);
        pso.dynamicStates.push_back(DynamicState::eViewPort);

        pso.colorBlendState = GfxPipelineColorBlendState::CreateDisabled();

        pso.rasterizationState.cullMode = rhi::PolygonCullMode::eBack;
        std::vector<ShaderResourceBinding> uboBindings;
        {
            ShaderResourceBinding binding0{};
            binding0.binding = 0;
            binding0.type    = ShaderResourceType::eUniformBuffer;
            binding0.handles.push_back(m_sceneUBO);
            uboBindings.emplace_back(std::move(binding0));
        }
        std::vector<ShaderResourceBinding> textureBindings;
        {
            ShaderResourceBinding binding{};
            binding.binding = 0;
            binding.type    = ShaderResourceType::eSamplerWithTexture;
            binding.handles.push_back(m_depthSampler);
            binding.handles.push_back(m_offscreenDepthTexture);
            textureBindings.emplace_back(std::move(binding));
        }
        rc::GraphicsPassBuilder builder(m_renderDevice);
        m_gfxPasses.sceneShadow =
            builder.AddShaderStage(rhi::ShaderStage::eVertex, "ShadowMapping/scene.vert.spv")
                .AddShaderStage(rhi::ShaderStage::eFragment, "ShadowMapping/scene.frag.spv")
                .SetNumSamples(SampleCount::e1)
                .AddColorRenderTarget(m_viewport->GetSwapchainFormat(),
                                      TextureUsage::eColorAttachment,
                                      m_viewport->GetColorBackBuffer())
                .SetDepthStencilTarget(
                    m_viewport->GetDepthStencilFormat(), m_viewport->GetDepthStencilBackBuffer(),
                    rhi::RenderTargetLoadOp::eClear, rhi::RenderTargetStoreOp::eStore)
                .SetShaderResourceBinding(0, uboBindings)
                .SetShaderResourceBinding(1, textureBindings)
                .SetPipelineState(pso)
                .SetFramebufferInfo(m_viewport)
                .SetTag("Lighting")
                .Build();
    }
}

void ShadowMappingApp::LoadResources()
{
    Application::LoadResources();

    m_scene         = MakeUnique<sg::Scene>();
    auto gltfLoader = MakeUnique<asset::FastGLTFLoader>();
    gltfLoader->LoadFromFile(m_modelPath, m_scene.Get());

    uint32_t vbSize = gltfLoader->GetVertices().size() * sizeof(asset::Vertex);
    m_vertexBuffer  = m_renderDevice->CreateVertexBuffer(
        vbSize, reinterpret_cast<const uint8_t*>(gltfLoader->GetVertices().data()));

    m_indexBuffer = m_renderDevice->CreateIndexBuffer(
        gltfLoader->GetIndices().size() * sizeof(uint32_t),
        reinterpret_cast<const uint8_t*>(gltfLoader->GetIndices().data()));

    UpdateUniformBufferData();

    m_offscreenUBO = m_renderDevice->CreateUniformBuffer(
        sizeof(OffscreenUniformData), reinterpret_cast<const uint8_t*>(&m_offscreenUniformData));

    m_sceneUBO = m_renderDevice->CreateUniformBuffer(
        sizeof(SceneUniformData), reinterpret_cast<const uint8_t*>(&m_sceneUniformData));
}

void ShadowMappingApp::UpdateUniformBufferData()
{
    //    m_sceneUniformData.lightPos.x = cos(glm::radians(m_animationTimer * 360.0f)) * 4.0f;
    //    m_sceneUniformData.lightPos.y = 5.0f + sin(glm::radians(m_animationTimer * 360.0f)) * 2.0f;
    //    m_sceneUniformData.lightPos.z = 2.0f + sin(glm::radians(m_animationTimer * 360.0f)) * 0.5f;
    m_sceneUniformData.lightPos.x = 0.80f;
    m_sceneUniformData.lightPos.y = 0.80f;
    m_sceneUniformData.lightPos.z = 0.8f;
    //    m_sceneUniformData.lightPos.w = 1.0f;

    // Matrix from light's point of view
    Mat4 depthProjectionMatrix = glm::perspective(glm::radians(m_lightFOV), 1.0f,
                                                  m_shadowMapConfig.zNear, m_shadowMapConfig.zFar);
    Mat4 depthViewMatrix  = glm::lookAt(m_sceneUniformData.lightPos, Vec3(0.0f), Vec3(0, 1.0f, 0));
    Mat4 depthModelMatrix = Mat4(1.0f);

    m_offscreenUniformData.depthMVP = depthProjectionMatrix * depthViewMatrix * depthModelMatrix;

    m_sceneUniformData.projection   = m_camera->GetProjectionMatrix();
    m_sceneUniformData.view         = m_camera->GetViewMatrix();
    m_sceneUniformData.model        = Mat4(1.0f);
    m_sceneUniformData.depthBiasMVP = m_offscreenUniformData.depthMVP;
    m_sceneUniformData.zNear        = m_shadowMapConfig.zNear;
    m_sceneUniformData.zFar         = m_shadowMapConfig.zFar;
}

void ShadowMappingApp::BuildRenderGraph()
{
    m_rdg = MakeUnique<rc::RenderGraph>();
    m_rdg->Begin();


    // offscreen pass
    {
        std::vector<RenderPassClearValue> clearValues(1);
        //        clearValues[0].color   = {0.2f, 0.2f, 0.2f, 1.0f};
        clearValues[0].depth   = 1.0f;
        clearValues[0].stencil = 0;

        Rect2 area;
        area.minX = 0;
        area.minY = 0;
        area.maxX = cShadowMapSize;
        area.maxY = cShadowMapSize;

        Rect2<float> vp;
        vp.minX = 0.0f;
        vp.minY = 0.0f;
        vp.maxX = cShadowMapSize;
        vp.maxY = cShadowMapSize;


        auto* pass = m_rdg->AddGraphicsPassNode(m_gfxPasses.offscreen, area, clearValues, true);
        m_rdg->DeclareTextureAccessForPass(
            pass, m_offscreenDepthTexture, TextureUsage::eDepthStencilAttachment,
            TextureSubResourceRange::DepthStencil(), rhi::AccessMode::eReadWrite);

        m_rdg->AddGraphicsPassSetScissorNode(pass, area);
        m_rdg->AddGraphicsPassBindVertexBufferNode(pass, m_vertexBuffer, {0});
        m_rdg->AddGraphicsPassBindIndexBufferNode(pass, m_indexBuffer, DataFormat::eR32UInt);
        m_rdg->AddGraphicsPassSetViewportNode(pass, vp);
        m_rdg->AddGraphicsPassSetDepthBiasNode(pass, m_shadowMapConfig.depthBiasConstant, 0.0f,
                                               m_shadowMapConfig.depthBiasSlope);
        for (auto* node : m_scene->GetRenderableNodes())
        {
            for (auto* subMesh : node->GetComponent<sg::Mesh>()->GetSubMeshes())
            {
                m_rdg->AddGraphicsPassDrawIndexedNode(pass, subMesh->GetIndexCount(), 1,
                                                      subMesh->GetFirstIndex(), 0, 0);
            }
        }
    }
    // scene shadow pass
    {
        std::vector<RenderPassClearValue> clearValues(2);
        clearValues[0].color   = {0.8f, 0.8f, 0.8f, 1.0f};
        clearValues[1].depth   = 1.0f;
        clearValues[1].stencil = 0;

        Rect2 area;
        area.minX = 0;
        area.minY = 0;
        area.maxX = static_cast<int>(m_window->GetExtent2D().width);
        area.maxY = static_cast<int>(m_window->GetExtent2D().height);

        Rect2<float> vp;
        vp.minX = 0.0f;
        vp.minY = 0.0f;
        vp.maxX = (float)m_window->GetExtent2D().width;
        vp.maxY = (float)m_window->GetExtent2D().height;

        auto* pass = m_rdg->AddGraphicsPassNode(m_gfxPasses.sceneShadow, area, clearValues, true);
        m_rdg->DeclareTextureAccessForPass(pass, m_offscreenDepthTexture, TextureUsage::eSampled,
                                           TextureSubResourceRange::DepthStencil(),
                                           rhi::AccessMode::eRead);
        m_rdg->AddGraphicsPassSetScissorNode(pass, area);
        m_rdg->AddGraphicsPassBindVertexBufferNode(pass, m_vertexBuffer, {0});
        m_rdg->AddGraphicsPassBindIndexBufferNode(pass, m_indexBuffer, DataFormat::eR32UInt);
        m_rdg->AddGraphicsPassSetViewportNode(pass, vp);
        for (auto* node : m_scene->GetRenderableNodes())
        {
            for (auto* subMesh : node->GetComponent<sg::Mesh>()->GetSubMeshes())
            {
                m_rdg->AddGraphicsPassDrawIndexedNode(pass, subMesh->GetIndexCount(), 1,
                                                      subMesh->GetFirstIndex(), 0, 0);
            }
        }
    }
    m_rdg->End();
}

ZEN_RHI_DEMO_MAIN(ShadowMappingApp, "shadowmapping")