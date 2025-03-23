#include "Gears.h"

GearsApp::GearsApp(const platform::WindowConfig& config) : Application(config)
{
    m_camera->SetPosition(Vec3(0.0f, 2.5f, 16.0f));
}

void GearsApp::GenerateGears()
{
    // Set up three differently shaped and colored gears
    std::vector<GearDefinition> gearDefinitions(3);

    // Large red gear
    gearDefinitions[0].innerRadius = 1.0f;
    gearDefinitions[0].outerRadius = 4.0f;
    gearDefinitions[0].width       = 1.0f;
    gearDefinitions[0].numTeeth    = 20;
    gearDefinitions[0].toothDepth  = 0.7f;
    gearDefinitions[0].color       = {1.0f, 0.0f, 0.0f};
    gearDefinitions[0].pos         = {-3.0f, 0.0f, 0.0f};
    gearDefinitions[0].rotSpeed    = 1.0f;
    gearDefinitions[0].rotOffset   = 0.0f;

    // Medium sized green gear
    gearDefinitions[1].innerRadius = 0.5f;
    gearDefinitions[1].outerRadius = 2.0f;
    gearDefinitions[1].width       = 2.0f;
    gearDefinitions[1].numTeeth    = 10;
    gearDefinitions[1].toothDepth  = 0.7f;
    gearDefinitions[1].color       = {0.0f, 1.0f, 0.2f};
    gearDefinitions[1].pos         = {3.1f, 0.0f, 0.0f};
    gearDefinitions[1].rotSpeed    = -2.0f;
    gearDefinitions[1].rotOffset   = -9.0f;

    // Small blue gear
    gearDefinitions[2].innerRadius = 1.3f;
    gearDefinitions[2].outerRadius = 2.0f;
    gearDefinitions[2].width       = 0.5f;
    gearDefinitions[2].numTeeth    = 10;
    gearDefinitions[2].toothDepth  = 0.7f;
    gearDefinitions[2].color       = {0.0f, 0.0f, 1.0f};
    gearDefinitions[2].pos         = {-3.1f, -6.2f, 0.0f};
    gearDefinitions[2].rotSpeed    = -2.0f;
    gearDefinitions[2].rotOffset   = -30.0f;

    // We'll be using a single vertex and a single index buffer for all the gears, no matter their number
    // This is a Vulkan best practice as it keeps the no. of memory/buffer allocations low
    // Vulkan offers all the tools to easily index into those buffers at a later point (see the buildCommandBuffers function)

    std::vector<Gear::Vertex> vertices{};
    std::vector<uint32_t> indices{};

    // Fills the vertex and index buffers for each of the gear
    m_gears.resize(gearDefinitions.size());
    for (int32_t i = 0; i < m_gears.size(); i++)
    {
        m_gears[i].generate(gearDefinitions[i], vertices, indices);
    }

    uint32_t vbSize = vertices.size() * sizeof(Gear::Vertex);
    m_vertexBuffer  = m_renderDevice->CreateVertexBuffer(
        vbSize, reinterpret_cast<const uint8_t*>(vertices.data()));

    m_indexBuffer = m_renderDevice->CreateIndexBuffer(
        indices.size() * sizeof(uint32_t), reinterpret_cast<const uint8_t*>(indices.data()));
}

void GearsApp::BuildGraphicsPasses()
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

    rc::GraphicsPassBuilder builder(m_renderDevice);
    m_gfxPass =
        builder.AddShaderStage(rhi::ShaderStage::eVertex, "gears.vert.spv")
            .AddShaderStage(rhi::ShaderStage::eFragment, "gears.frag.spv")
            .SetNumSamples(SampleCount::e1)
            .AddColorRenderTarget(m_viewport->GetSwapchainFormat(), TextureUsage::eColorAttachment,
                                  m_viewport->GetColorBackBuffer())
            .SetDepthStencilTarget(m_viewport->GetDepthStencilFormat(),
                                   m_viewport->GetDepthStencilBackBuffer())
            .SetShaderResourceBinding(0, uboBindings)
            .SetPipelineState(pso)
            .SetFramebufferInfo(m_viewport, m_viewport->GetWidth(), m_viewport->GetHeight())
            .Build();
}

void GearsApp::UpdateSceneData()
{
    float degree = 360.f * m_animationTimer;

    // Camera specific global matrices
    m_sceneData.lightPos = glm::vec4(0.0f, 0.0f, 2.5f, 1.0f);

    // Update the model matrix for each gear that contains it's position and rotation
    for (auto i = 0; i < NUM_GEARS; i++)
    {
        Gear gear             = m_gears[i];
        m_sceneData.models[i] = glm::mat4(1.0f);
        m_sceneData.models[i] = glm::translate(m_sceneData.models[i], gear.pos);
        m_sceneData.models[i] = glm::rotate(m_sceneData.models[i],
                                            glm::radians((gear.rotSpeed * degree) + gear.rotOffset),
                                            glm::vec3(0.0f, 0.0f, 1.0f));
    }
}

void GearsApp::LoadResources()
{
    Application::LoadResources();
    GenerateGears();
    UpdateSceneData();
    m_sceneUBO = m_renderDevice->CreateUniformBuffer(
        sizeof(SceneData), reinterpret_cast<const uint8_t*>(&m_sceneData));
}

void GearsApp::BuildRenderGraph()
{
    m_rdg = MakeUnique<rc::RenderGraph>();
    m_rdg->Begin();
    Rect2 area;
    area.minX = 0;
    area.minY = 0;
    area.maxX = (int)m_window->GetExtent2D().width;
    area.maxY = (int)m_window->GetExtent2D().height;

    Rect2<float> vp;
    vp.minX = 0.0f;
    vp.minY = 0.0f;
    vp.maxX = (float)m_window->GetExtent2D().width;
    vp.maxY = (float)m_window->GetExtent2D().height;
    std::vector<RenderPassClearValue> clearValues(2);
    clearValues[0].color   = {0.2f, 0.2f, 0.2f, 1.0f};
    clearValues[1].depth   = 1.0f;
    clearValues[1].stencil = 1.0f;

    auto* mainPass = m_rdg->AddGraphicsPassNode(m_gfxPass, area, clearValues, true);

    m_rdg->AddGraphicsPassBindVertexBufferNode(mainPass, m_vertexBuffer, {0});
    m_rdg->AddGraphicsPassBindIndexBufferNode(mainPass, m_indexBuffer, DataFormat::eR32UInt);
    m_rdg->AddGraphicsPassSetViewportNode(mainPass, vp);
    m_rdg->AddGraphicsPassSetScissorNode(mainPass, area);
    for (auto i = 0; i < NUM_GEARS; i++)
    {
        m_rdg->AddGraphicsPassDrawIndexedNode(mainPass, m_gears[i].indexCount, 1,
                                              m_gears[i].indexStart, 0, i);
    }
    m_rdg->End();
}

void GearsApp::Prepare()
{
    Application::Prepare();
    LoadResources();
    BuildGraphicsPasses();
    BuildRenderGraph();
}

void GearsApp::Run()
{
    while (!m_window->ShouldClose())
    {
        auto frameTime = static_cast<float>(m_timer->Tick());
        m_animationTimer += frameTime * m_animationSpeed;
        if (m_animationTimer > 1.0f)
        {
            m_animationTimer -= 1.0f;
        }
        m_window->Update();
        m_camera->Update(frameTime);
        UpdateSceneData();
        m_renderDevice->UpdateBuffer(m_sceneUBO, sizeof(SceneData),
                                     reinterpret_cast<const uint8_t*>(&m_sceneData));
        m_renderDevice->ExecuteFrame(m_viewport, m_rdg.Get());
        m_renderDevice->NextFrame();
    }
}

void GearsApp::Destroy()
{
    Application::Destroy();
}

ZEN_RHI_DEMO_MAIN(GearsApp, "gears")