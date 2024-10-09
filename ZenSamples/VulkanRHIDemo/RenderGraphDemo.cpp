#include "RenderGraphDemo.h"

void Application::BuildPipeline()
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
    ShaderResourceBinding binding{};
    binding.binding = 0;
    binding.type    = ShaderResourceType::eUniformBuffer;
    binding.handles.push_back(m_cameraUBO);
    bindings.emplace_back(std::move(binding));

    rc::RenderPipelineBuilder builder;
    m_mainRP =
        builder.Begin(m_renderDevice)
            .SetVertexShader("triangle.vert.spv")
            .SetFragmentShader("triangle_fixed.frag.spv")
            .SetNumSamples(SampleCount::e1)
            .SetNumRenderTargets(1)
            .AddColorRenderTarget(m_viewport->GetSwapchainFormat(), TextureUsage::eColorAttachment)
            .SetFramebufferInfo(fbInfo)
            .SetShaderResourceBinding(0, bindings)
            .SetPipelineState(pso)
            .Build();
}

void Application::BuildResources()
{
    // buffers
    std::vector<Vertex> vertices = {
        {Vec3(0.5f, 0.5f, 1.0f), Vec3(1.0f, 0.0f, 0.0f), Vec2(0.0f, 1.0f)},
        {Vec3(0.5f, -0.5f, 1.0f), Vec3(0.0f, 1.0f, 0.0f), Vec2(1.0f, 1.0f)},
        {Vec3(-0.5f, 0.5f, 1.0f), Vec3(0.0f, 0.0f, 1.0f), Vec2(0.0f, 0.0f)}};
    std::vector<uint32_t> indices = {0, 1, 2};

    uint32_t vbSize = vertices.size() * sizeof(Vertex);
    m_vertexBuffer  = m_renderDevice->CreateVertexBuffer(
        vbSize, reinterpret_cast<const uint8_t*>(vertices.data()));

    m_indexBuffer = m_renderDevice->CreateIndexBuffer(
        indices.size() * sizeof(uint32_t), reinterpret_cast<const uint8_t*>(indices.data()));

    m_cameraUBO = m_renderDevice->CreateUniformBuffer(
        sizeof(CameraUniformData), reinterpret_cast<const uint8_t*>(&m_cameraData));
}

void Application::BuildRenderGraph()
{
    m_rdg = MakeUnique<rc::RenderGraph>();
    m_rdg->Begin();
    Rect2 area;
    area.minX = 0;
    area.minY = 0;
    area.maxX = (int)m_window->GetExtent2D().width;
    area.maxY = (int)m_window->GetExtent2D().height;
    RenderPassClearValue clearValue;
    clearValue.color = {0.2f, 0.2f, 0.2f, 1.0f};

    FramebufferHandle framebuffer = m_viewport->GetCompatibleFramebuffer(m_mainRP.renderPass);
    auto* mainPass =
        m_rdg->AddGraphicsPassNode(m_mainRP.renderPass, framebuffer, area, clearValue, true);
    m_rdg->AddGraphicsPassBindPipelineNode(mainPass, m_mainRP.pipeline, PipelineType::eGraphics);
    m_rdg->AddGraphicsPassBindVertexBufferNode(mainPass, m_vertexBuffer, {0});
    m_rdg->AddGraphicsPassBindIndexBufferNode(mainPass, m_indexBuffer, DataFormat::eR32UInt);
    m_rdg->AddGraphicsPassSetViewportNode(mainPass, area);
    m_rdg->AddGraphicsPassSetScissorNode(mainPass, area);
    m_rdg->AddGraphicsPassDrawNode(mainPass, 3, 1);
    m_rdg->End();
}

Application::Application()
{
    m_renderDevice = new rc::RenderDevice(GraphicsAPIType::eVulkan, 3);
    m_renderDevice->Init();

    platform::WindowConfig windowConfig{"RDGV2Demo", true, 1280, 720};
    m_window = new platform::GlfwWindowImpl(windowConfig);
    m_window->SetOnResize([&](uint32_t width, uint32_t height) {
        m_renderDevice->ResizeViewport(&m_viewport, m_window, width, height);
        BuildRenderGraph();
        // recreate camera
        m_camera = sys::Camera::CreateUnique(Vec3{0.0f, 0.0f, -0.1f}, Vec3{0.0f, 0.0f, 0.0f},
                                             m_window->GetAspect());
    });
    m_viewport =
        m_renderDevice->CreateViewport(m_window, windowConfig.width, windowConfig.height, true);

    m_camera = sys::Camera::CreateUnique(Vec3{0.0f, 0.0f, -0.1f}, Vec3{0.0f, 0.0f, 0.0f},
                                         m_window->GetAspect());
    m_cameraData.projViewMatrix = m_camera->GetProjectionMatrix() * m_camera->GetViewMatrix();

    m_timer = MakeUnique<platform::Timer>();
}

void Application::Prepare()
{
    BuildResources();
    BuildPipeline();
    BuildRenderGraph();
}

void Application::Destroy()
{
    m_rdg->Destroy();
    m_renderDevice->Destroy();
    delete m_renderDevice;
}

void Application::Run()
{
    while (!m_window->ShouldClose())
    {
        m_window->Update();
        m_camera->Update(static_cast<float>(m_timer->Tick()));
        m_cameraData.projViewMatrix = m_camera->GetProjectionMatrix() * m_camera->GetViewMatrix();
        m_renderDevice->Updatebuffer(m_cameraUBO, sizeof(CameraUniformData),
                                     reinterpret_cast<const uint8_t*>(&m_cameraData));
        m_renderDevice->ExecuteFrame(m_viewport, m_rdg.Get());
        m_renderDevice->NextFrame();
    }
}

int main(int argc, char** argv)
{
    Application application;
    application.Prepare();

    application.Run();

    application.Destroy();
}