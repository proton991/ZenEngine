#include "HelloTriangle.h"

HelloTriangleApp::HelloTriangleApp(const platform::WindowConfig& windowConfig) :
    Application(windowConfig, sys::CameraType::eOrbit)
{
    m_camera->SetPosition({0.0f, 0.0f, -2.0f});
}

void HelloTriangleApp::BuildRenderPipeline()
{
    GfxPipelineStates pso{};
    pso.primitiveType      = DrawPrimitiveType::eTriangleList;
    pso.rasterizationState = {};
    pso.colorBlendState    = GfxPipelineColorBlendState::CreateDisabled();
    pso.depthStencilState  = {};
    pso.multiSampleState   = {};
    pso.dynamicStates.push_back(DynamicState::eScissor);
    pso.dynamicStates.push_back(DynamicState::eViewPort);

    TextureHandle renderBackBuffer = m_viewport->GetRenderBackBuffer();
    FramebufferInfo fbInfo{};
    fbInfo.width           = m_window->GetExtent2D().width;
    fbInfo.height          = m_window->GetExtent2D().height;
    fbInfo.numRenderTarget = 1;
    fbInfo.renderTargets   = &renderBackBuffer;

    std::vector<ShaderResourceBinding> bindings;
    {
        ShaderResourceBinding binding{};
        binding.binding = 0;
        binding.type    = ShaderResourceType::eUniformBuffer;
        binding.handles.push_back(m_cameraUBO);
        bindings.emplace_back(std::move(binding));
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

    rc::RenderPipelineBuilder builder(m_renderDevice);
    m_mainRP =
        builder.SetVertexShader("triangle.vert.spv")
            .SetFragmentShader("triangle_fixed.frag.spv")
            .SetNumSamples(SampleCount::e1)
            .AddColorRenderTarget(m_viewport->GetSwapchainFormat(), TextureUsage::eColorAttachment)
            .SetShaderResourceBinding(0, bindings)
            .SetShaderResourceBinding(1, textureBindings)
            .SetPipelineState(pso)
            .Build();
}

void HelloTriangleApp::LoadResources()
{
    Application::LoadResources();
    // buffers
    std::vector<Vertex> vertices = {
        {Vec3(0.5f, 0.5f, 0.0f), Vec3(1.0f, 0.0f, 0.0f), Vec2(0.0f, 1.0f)},
        {Vec3(0.5f, -0.5f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), Vec2(1.0f, 1.0f)},
        {Vec3(-0.5f, 0.5f, 0.0f), Vec3(0.0f, 0.0f, 1.0f), Vec2(0.0f, 0.0f)}};
    std::vector<uint32_t> indices = {0, 1, 2};

    uint32_t vbSize = vertices.size() * sizeof(Vertex);
    m_vertexBuffer  = m_renderDevice->CreateVertexBuffer(
        vbSize, reinterpret_cast<const uint8_t*>(vertices.data()));

    m_indexBuffer = m_renderDevice->CreateIndexBuffer(
        indices.size() * sizeof(uint32_t), reinterpret_cast<const uint8_t*>(indices.data()));

    // load texture
    SamplerInfo samplerInfo{};
    m_sampler = m_renderDevice->CreateSampler(samplerInfo);
    m_texture = m_renderDevice->RequestTexture2D("wood.png");
}

void HelloTriangleApp::BuildRenderGraph()
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

    RenderPassClearValue clearValue;
    clearValue.color = {0.2f, 0.2f, 0.2f, 1.0f};

    FramebufferHandle framebuffer = m_viewport->GetCompatibleFramebuffer(m_mainRP.renderPass);
    auto* mainPass =
        m_rdg->AddGraphicsPassNode(m_mainRP.renderPass, framebuffer, area, clearValue, true);
    m_rdg->AddGraphicsPassBindPipelineNode(mainPass, m_mainRP.pipeline, PipelineType::eGraphics);
    m_rdg->AddGraphicsPassBindVertexBufferNode(mainPass, m_vertexBuffer, {0});
    m_rdg->AddGraphicsPassBindIndexBufferNode(mainPass, m_indexBuffer, DataFormat::eR32UInt);
    m_rdg->AddGraphicsPassSetViewportNode(mainPass, vp);
    m_rdg->AddGraphicsPassSetScissorNode(mainPass, area);
    m_rdg->AddGraphicsPassDrawNode(mainPass, 3, 1);
    m_rdg->End();
}

void HelloTriangleApp::Prepare()
{
    Application::Prepare();
    LoadResources();
    BuildRenderPipeline();
    BuildRenderGraph();
}

void HelloTriangleApp::Destroy()
{
    Application::Destroy();
}

void HelloTriangleApp::Run()
{
    while (!m_window->ShouldClose())
    {
        m_window->Update();
        m_camera->Update(static_cast<float>(m_timer->Tick()));
        m_renderDevice->ExecuteFrame(m_viewport, m_rdg.Get());
        m_renderDevice->NextFrame();
    }
}

int main(int argc, char** argv)
{
    platform::WindowConfig windowConfig{"hello_triangle", true, 1280, 720};
    Application* app = new HelloTriangleApp(windowConfig);

    app->Prepare();

    app->Run();

    app->Destroy();

    delete app;
}