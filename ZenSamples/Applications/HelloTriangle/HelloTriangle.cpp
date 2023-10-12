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

    m_cameraUniformBuffer = UniformBuffer::CreateUnique(*m_device, sizeof(CameraUniformData));

    SetupRenderGraph();

    LoadModel();

    m_camera = sys::Camera::CreateUnique(Vec3{0.0f, 0.0f, -0.1f}, Vec3{0.0f, 0.0f, 0.0f}, m_window->GetAspect());
    m_timer  = MakeUnique<platform::Timer>();
}

void HelloTriangle::Update(float deltaTime)
{
    Application::Update(deltaTime);
    m_camera->Update(deltaTime);
    m_cameraUniformData.projViewMatrix = m_camera->GetProjectionMatrix() * m_camera->GetViewMatrix();
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
    mainPass->ReadFromExternalBuffer("cameraUniform", m_cameraUniformBuffer.Get());
    mainPass->BindSRD("cameraUniform", VK_SHADER_STAGE_VERTEX_BIT, "uCameraData");
    mainPass->SetOnExecute([&](val::CommandBuffer* commandBuffer) {
        auto extent = m_renderContext->GetSwapchainExtent2D();
        commandBuffer->SetViewport(static_cast<float>(extent.width), static_cast<float>(extent.height));
        commandBuffer->SetScissor(extent.width, extent.height);
        commandBuffer->BindVertexBuffers(*m_vertexBuffer);
        commandBuffer->BindIndexBuffer(*m_indexBuffer, VK_INDEX_TYPE_UINT32);
        commandBuffer->DrawIndices(m_indexBuffer->GetIndexCount(), 1);
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
        Update(m_timer->Tick());
        auto* commandBuffer = m_renderContext->StartFrame(val::CommandPool::ResetMode::ResetPool);
        m_renderContext->UpdateUniformBuffer<CameraUniformData>(&m_cameraUniformData, m_cameraUniformBuffer.Get(), commandBuffer);
        m_renderGraph->Execute(commandBuffer, m_renderContext.Get());
        m_renderContext->EndFrame();
    }
}

void HelloTriangle::LoadModel()
{
    std::vector<Vertex> vertices = {
        {Vec3(0.5f, 0.5f, 1.0f), Vec3(1.0f, 0.0f, 0.0f)},
        {Vec3(0.5f, -0.5f, 1.0f), Vec3(0.0f, 1.0f, 0.0f)},
        {Vec3(-0.5f, 0.5f, 1.0f), Vec3(0.0f, 0.0f, 1.0f)}};
    std::vector<uint32_t> indices = {0, 1, 2};

    m_vertexBuffer = VertexBuffer::CreateUnique(*m_device, GetArrayViewSize(MakeView(vertices)));
    m_indexBuffer  = IndexBuffer::CreateUnique(*m_device, MakeView(indices));

    auto* stagingBuffer      = m_renderContext->GetCurrentStagingBuffer();
    auto  verticesSubmitInfo = stagingBuffer->Submit(MakeView(vertices));
    auto  indicesSubmitInfo  = stagingBuffer->Submit(MakeView(indices));
    auto* cmdBuffer          = m_renderContext->GetCommandBuffer();
    cmdBuffer->Begin();
    cmdBuffer->CopyBuffer(stagingBuffer, verticesSubmitInfo.offset, m_vertexBuffer.Get(), 0, verticesSubmitInfo.size);
    cmdBuffer->CopyBuffer(stagingBuffer, indicesSubmitInfo.offset, m_indexBuffer.Get(), 0, indicesSubmitInfo.size);
    cmdBuffer->End();
    m_renderContext->SubmitImmediate(cmdBuffer);
    m_renderContext->ResetCommandPool();
}
} // namespace zen
