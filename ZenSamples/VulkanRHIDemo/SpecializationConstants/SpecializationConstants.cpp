#include "SpecializationConstants.h"
#include "AssetLib/FastGLTFLoader.h"

SpecializationConstantsApp::SpecializationConstantsApp(const platform::WindowConfig& windowConfig) :
    Application(windowConfig, sg::CameraType::eFirstPerson)
{
    m_camera->SetPosition({1.0f, 1.0f, -2.0f});
}

void SpecializationConstantsApp::Prepare()
{
    Application::Prepare();
    LoadResources();
    BuildGraphicsPasses();
    BuildRenderGraph();
}

void SpecializationConstantsApp::Run()
{
    while (!m_window->ShouldClose())
    {
        auto frameTime = static_cast<float>(m_timer->Tick());
        m_window->Update();
        m_camera->Update(frameTime);
        m_renderDevice->ExecuteFrame(m_viewport, m_rdg.Get());
        m_renderDevice->NextFrame();
    }
}

void SpecializationConstantsApp::Destroy()
{
    Application::Destroy();
}

void SpecializationConstantsApp::BuildGraphicsPasses()
{
    GfxPipelineStates pso{};
    pso.primitiveType = DrawPrimitiveType::eTriangleList;

    pso.rasterizationState          = {};
    pso.rasterizationState.cullMode = PolygonCullMode::eBack;

    pso.colorBlendState   = GfxPipelineColorBlendState::CreateDisabled();
    pso.depthStencilState = {};
    pso.multiSampleState  = {};
    pso.dynamicStates.push_back(DynamicState::eScissor);
    pso.dynamicStates.push_back(DynamicState::eViewPort);

    std::vector<ShaderResourceBinding> uboBindings;
    {
        ShaderResourceBinding binding0{};
        binding0.binding = 0;
        binding0.type    = ShaderResourceType::eUniformBuffer;
        binding0.handles.push_back(m_cameraUBO);
        uboBindings.emplace_back(std::move(binding0));

        ShaderResourceBinding binding1{};
        binding1.binding = 1;
        binding1.type    = ShaderResourceType::eUniformBuffer;
        binding1.handles.push_back(m_sceneUBO);
        uboBindings.emplace_back(std::move(binding1));
    }
    std::vector<ShaderResourceBinding> textureBindings;
    {
        ShaderResourceBinding binding{};
        binding.binding = 0;
        binding.type    = ShaderResourceType::eSamplerWithTexture;
        binding.handles.push_back(m_sampler);
        binding.handles.push_back(m_texture);
        textureBindings.emplace_back(std::move(binding));
    }
    // phone
    {
        rc::GraphicsPassBuilder builder(m_renderDevice);
        m_gfxPasses.phong = builder.AddShaderStage(rhi::ShaderStage::eVertex, "uber.vert.spv")
                                .AddShaderStage(rhi::ShaderStage::eFragment, "uber.frag.spv")
                                .SetShaderSpecializationConstants(0, 0)
                                .SetShaderSpecializationConstants(1, 0.0f)
                                .SetNumSamples(SampleCount::e1)
                                .AddColorRenderTarget(m_viewport->GetSwapchainFormat(),
                                                      TextureUsage::eColorAttachment,
                                                      m_viewport->GetColorBackBuffer())
                                .SetDepthStencilTarget(m_viewport->GetDepthStencilFormat(),
                                                       m_viewport->GetDepthStencilBackBuffer())
                                .SetShaderResourceBinding(0, uboBindings)
                                .SetShaderResourceBinding(1, textureBindings)
                                .SetPipelineState(pso)
                                .SetFramebufferInfo(m_viewport)
                                .SetTag("Phone")
                                .Build();
    }
    // toon
    {
        rc::GraphicsPassBuilder builder(m_renderDevice);
        m_gfxPasses.toon = builder.AddShaderStage(rhi::ShaderStage::eVertex, "uber.vert.spv")
                               .AddShaderStage(rhi::ShaderStage::eFragment, "uber.frag.spv")
                               .SetShaderSpecializationConstants(0, 1)
                               .SetShaderSpecializationConstants(1, 0.0f)
                               .SetNumSamples(SampleCount::e1)
                               .AddColorRenderTarget(m_viewport->GetSwapchainFormat(),
                                                     TextureUsage::eColorAttachment,
                                                     m_viewport->GetColorBackBuffer())
                               .SetDepthStencilTarget(m_viewport->GetDepthStencilFormat(),
                                                      m_viewport->GetDepthStencilBackBuffer())
                               .SetShaderResourceBinding(0, uboBindings)
                               .SetShaderResourceBinding(1, textureBindings)
                               .SetPipelineState(pso)
                               .SetFramebufferInfo(m_viewport)
                               .SetTag("Toon")
                               .Build();
    }
    // textured
    {
        rc::GraphicsPassBuilder builder(m_renderDevice);
        m_gfxPasses.textured = builder.AddShaderStage(rhi::ShaderStage::eVertex, "uber.vert.spv")
                                   .AddShaderStage(rhi::ShaderStage::eFragment, "uber.frag.spv")
                                   .SetShaderSpecializationConstants(0, 2)
                                   .SetShaderSpecializationConstants(1, 0.0f)
                                   .SetNumSamples(SampleCount::e1)
                                   .AddColorRenderTarget(m_viewport->GetSwapchainFormat(),
                                                         TextureUsage::eColorAttachment,
                                                         m_viewport->GetColorBackBuffer())
                                   .SetDepthStencilTarget(m_viewport->GetDepthStencilFormat(),
                                                          m_viewport->GetDepthStencilBackBuffer())
                                   .SetShaderResourceBinding(0, uboBindings)
                                   .SetShaderResourceBinding(1, textureBindings)
                                   .SetPipelineState(pso)
                                   .SetFramebufferInfo(m_viewport)
                                   .SetTag("Textured")
                                   .Build();
    }
}

void SpecializationConstantsApp::LoadResources()
{
    Application::LoadResources();
    // load texture
    SamplerInfo samplerInfo{};
    m_sampler = m_renderDevice->CreateSampler(samplerInfo);
    m_texture = m_renderDevice->LoadTexture2D("wood.png");

    m_scene         = MakeUnique<sg::Scene>();
    auto gltfLoader = MakeUnique<asset::FastGLTFLoader>();
    gltfLoader->LoadFromFile(m_modelPath, m_scene.Get());

    uint32_t vbSize = gltfLoader->GetVertices().size() * sizeof(asset::Vertex);
    m_vertexBuffer  = m_renderDevice->CreateVertexBuffer(
        vbSize, reinterpret_cast<const uint8_t*>(gltfLoader->GetVertices().data()));

    m_indexBuffer = m_renderDevice->CreateIndexBuffer(
        gltfLoader->GetIndices().size() * sizeof(uint32_t),
        reinterpret_cast<const uint8_t*>(gltfLoader->GetIndices().data()));

    m_sceneData.lightPos = {0.0f, 2.0f, 1.0f, 0.0f};
    m_sceneData.modelMat = Mat4(1.0f);
    m_sceneUBO           = m_renderDevice->CreateUniformBuffer(
        sizeof(SceneData), reinterpret_cast<const uint8_t*>(&m_sceneData));
}

void SpecializationConstantsApp::BuildRenderGraph()
{
    m_rdg = MakeUnique<rc::RenderGraph>();
    m_rdg->Begin();
    Rect2 area;
    area.minX = 0;
    area.minY = 0;
    area.maxX = (int)m_window->GetExtent2D().width;
    area.maxY = (int)m_window->GetExtent2D().height;

    std::vector<RenderPassClearValue> clearValues(2);
    clearValues[0].color   = {0.2f, 0.2f, 0.2f, 1.0f};
    clearValues[1].depth   = 1.0f;
    clearValues[1].stencil = 1.0f;

    Rect2<float> leftVP;
    leftVP.minX = 0.0f;
    leftVP.minY = 0.0f;
    leftVP.maxX = (float)m_window->GetExtent2D().width / 3.0f;
    leftVP.maxY = (float)m_window->GetExtent2D().height;

    auto* mainPass = m_rdg->AddGraphicsPassNode(
        m_gfxPasses.phong.renderPass, m_gfxPasses.phong.framebuffer, area, clearValues, true);
    m_rdg->AddGraphicsPassSetScissorNode(mainPass, area);
    // phone
    m_rdg->AddPassBindPipelineNode(mainPass, m_gfxPasses.phong.pipeline, PipelineType::eGraphics);
    m_rdg->AddGraphicsPassBindVertexBufferNode(mainPass, m_vertexBuffer, {0});
    m_rdg->AddGraphicsPassBindIndexBufferNode(mainPass, m_indexBuffer, DataFormat::eR32UInt);
    m_rdg->AddGraphicsPassSetViewportNode(mainPass, leftVP);
    for (auto* node : m_scene->GetRenderableNodes())
    {
        for (auto* subMesh : node->GetComponent<sg::Mesh>()->GetSubMeshes())
        {
            m_rdg->AddGraphicsPassDrawIndexedNode(mainPass, subMesh->GetIndexCount(), 1,
                                                  subMesh->GetFirstIndex(), 0, 0);
        }
    }
    // toon
    Rect2<float> middleVP;
    middleVP.minX = (float)m_window->GetExtent2D().width / 3.0f;
    middleVP.minY = 0.0f;
    middleVP.maxX = 2 * (float)m_window->GetExtent2D().width / 3.0f;
    middleVP.maxY = (float)m_window->GetExtent2D().height;

    m_rdg->AddPassBindPipelineNode(mainPass, m_gfxPasses.toon.pipeline, PipelineType::eGraphics);
    m_rdg->AddGraphicsPassBindVertexBufferNode(mainPass, m_vertexBuffer, {0});
    m_rdg->AddGraphicsPassBindIndexBufferNode(mainPass, m_indexBuffer, DataFormat::eR32UInt);
    m_rdg->AddGraphicsPassSetViewportNode(mainPass, middleVP);

    for (auto* node : m_scene->GetRenderableNodes())
    {
        for (auto* subMesh : node->GetComponent<sg::Mesh>()->GetSubMeshes())
        {
            m_rdg->AddGraphicsPassDrawIndexedNode(mainPass, subMesh->GetIndexCount(), 1,
                                                  subMesh->GetFirstIndex(), 0, 0);
        }
    }
    // textured
    Rect2<float> rightVP;
    rightVP.minX = 2 * (float)m_window->GetExtent2D().width / 3.0f;
    rightVP.minY = 0.0f;
    rightVP.maxX = (float)m_window->GetExtent2D().width;
    rightVP.maxY = (float)m_window->GetExtent2D().height;

    m_rdg->AddPassBindPipelineNode(mainPass, m_gfxPasses.textured.pipeline,
                                   PipelineType::eGraphics);
    m_rdg->AddGraphicsPassBindVertexBufferNode(mainPass, m_vertexBuffer, {0});
    m_rdg->AddGraphicsPassBindIndexBufferNode(mainPass, m_indexBuffer, DataFormat::eR32UInt);
    m_rdg->AddGraphicsPassSetViewportNode(mainPass, rightVP);

    for (auto* node : m_scene->GetRenderableNodes())
    {
        for (auto* subMesh : node->GetComponent<sg::Mesh>()->GetSubMeshes())
        {
            m_rdg->AddGraphicsPassDrawIndexedNode(mainPass, subMesh->GetIndexCount(), 1,
                                                  subMesh->GetFirstIndex(), 0, 0);
        }
    }
    m_rdg->End();
}

ZEN_RHI_DEMO_MAIN(SpecializationConstantsApp, "specialization_constants")