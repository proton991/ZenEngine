#include "HelloTriangle.h"

namespace zen
{
HelloTriangle::HelloTriangle()
{
}

void HelloTriangle::Prepare(const platform::WindowConfig& windowConfig)
{
    Application::Prepare(windowConfig);

    // set up render device
    m_renderDevice  = MakeUnique<RenderDevice>(*m_device);
    m_renderContext = MakeUnique<RenderContext>(*m_device, m_window.Get());
    m_renderGraph   = MakeUnique<RenderGraph>(*m_renderDevice);
    m_windowHeight  = windowConfig.height;
    m_windowWidth   = windowConfig.width;
    m_shaderManager = MakeUnique<ShaderManager>(*m_device);

    SetupRenderGraph();

    LoadModel();
}

void HelloTriangle::Update(float deltaTime)
{
    Application::Update(deltaTime);
}

void HelloTriangle::SetupRenderGraph()
{
    val::ShaderModule* vertexShader = m_shaderManager->RequestShader("triangle.vert.spv", VK_SHADER_STAGE_VERTEX_BIT, {});
    val::ShaderModule* fragShader   = m_shaderManager->RequestShader("triangle_fixed.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT, {});

    RDGPass* mainPass = m_renderGraph->AddPass("MainPass", RDG_QUEUE_GRAPHICS_BIT);
    mainPass->UseShaders({vertexShader, fragShader});
    RDGImage::Info outputImgInfo{};
    outputImgInfo.format = m_renderContext->GetSwapchainFormat();
    outputImgInfo.usage  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    mainPass->WriteToColorImage("Output", outputImgInfo);
    mainPass->SetOnExecute([&](val::CommandBuffer* commandBuffer) {
        auto extent = m_renderContext->GetSwapchainExtent2D();
        commandBuffer->SetViewport(static_cast<float>(extent.width), static_cast<float>(extent.height));
        commandBuffer->SetScissor(extent.width, extent.height);
        commandBuffer->BindVertexBuffers(*m_vertexBuffer);
        commandBuffer->DrawVertices(3, 1);
    });
    m_renderGraph->SetBackBufferTag("Output");
    m_renderGraph->SetBackBufferSize(m_windowWidth, m_windowHeight);

    m_renderGraph->Compile();
}

void HelloTriangle::Run()
{
    while (!m_window->ShouldClose())
    {
        m_window->Update();
        auto* commandBuffer = m_renderContext->StartFrame(val::CommandPool::ResetMode::ResetPool);
        m_renderGraph->Execute(commandBuffer, m_renderContext.Get());
        m_renderContext->EndFrame();
    }
}

void HelloTriangle::LoadModel()
{
    std::vector<Vertex> vertices = {
        {Vec3(0.5f, 0.5f, 0.0f), Vec3(1.0f, 0.0f, 0.0f)},
        {Vec3(0.5f, -0.5f, 0.0f), Vec3(0.0f, 1.0f, 0.0f)},
        {Vec3(-0.5f, 0.5f, 0.0f), Vec3(0.0f, 0.0f, 1.0f)}};
    m_vertexBuffer = MakeUnique<VertexBuffer>(*m_device, GetArrayViewSize(MakeView(vertices)));

    auto* stagingBuffer = m_renderContext->GetCurrentStagingBuffer();
    auto  submitInfo    = stagingBuffer->Submit(MakeView(vertices));
    auto* cmdBuffer     = m_renderContext->GetCommandBuffer();
    cmdBuffer->Begin();
    cmdBuffer->CopyBuffer(stagingBuffer, submitInfo.offset, m_vertexBuffer.Get(), 0, submitInfo.size);
    cmdBuffer->End();
    m_renderContext->SubmitImmediate(cmdBuffer);
    m_renderContext->ResetCommandPool();
}
} // namespace zen
