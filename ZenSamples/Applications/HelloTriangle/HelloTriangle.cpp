#include "HelloTriangle.h"

namespace zen
{
HelloTriangle::HelloTriangle() {}

void HelloTriangle::Prepare(const platform::WindowConfig& windowConfig)
{
    Application::Prepare(windowConfig);

    // set up render device
    m_renderDevice  = MakeUnique<RenderDevice>(*m_device);
    m_renderContext = MakeUnique<RenderContext>(*m_device, m_window.Get());
    m_renderGraph   = MakeUnique<RenderGraph>(*m_renderDevice);
    m_shaderManager = MakeUnique<ShaderManager>(*m_device);

    m_textureManager = MakeUnique<TextureManager>(*m_device, *m_renderContext);

    m_cameraUniformBuffer = UniformBuffer::CreateUnique(*m_device, sizeof(CameraUniformData));

    LoadTexture();

    SetupRenderGraph();

    LoadModel();

    m_camera = sg::Camera::CreateUnique(Vec3{0.0f, 0.0f, -0.1f}, Vec3{0.0f, 0.0f, 0.0f},
                                         m_window->GetAspect());
    m_timer  = MakeUnique<platform::Timer>();
    m_window->SetOnResize([&](uint32_t width, uint32_t height) {
        m_renderContext->RecreateSwapchain(width, height);
        m_renderGraph = MakeUnique<RenderGraph>(*m_renderDevice);
        SetupRenderGraph();
    });
}

void HelloTriangle::Update(float deltaTime)
{
    Application::Update(deltaTime);
    m_camera->Update(deltaTime);
    m_cameraUniformData.projViewMatrix =
        m_camera->GetProjectionMatrix() * m_camera->GetViewMatrix();
}

void HelloTriangle::SetupRenderGraph()
{
    val::ShaderModule* pVertexShader =
        m_shaderManager->RequestShader("triangle.vert.spv", VK_SHADER_STAGE_VERTEX_BIT, {});
    val::ShaderModule* pFragShader =
        m_shaderManager->RequestShader("triangle_fixed.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT, {});

    RDGPass* pMainPass = m_renderGraph->AddPass("MainPass", RDG_QUEUE_GRAPHICS_BIT);
    pMainPass->UseShaders({pVertexShader, pFragShader});
    RDGImage::Info outputImgInfo{};
    outputImgInfo.format = m_renderContext->GetSwapchainFormat();
    pMainPass->WriteToColorImage("Output", outputImgInfo);
    pMainPass->ReadFromExternalImage("texture", m_pSimpleTexture);
    pMainPass->ReadFromExternalBuffer("cameraUniform", m_cameraUniformBuffer.Get());
    pMainPass->BindSRD("cameraUniform", VK_SHADER_STAGE_VERTEX_BIT, "uCameraData");
    pMainPass->BindSRD("texture", VK_SHADER_STAGE_FRAGMENT_BIT, "uTexture");
    pMainPass->BindSampler("texture", m_sampler.Get());
    pMainPass->SetOnExecute([&](val::CommandBuffer* pCommandBuffer) {
        auto extent = m_renderContext->GetSwapchainExtent2D();
        pCommandBuffer->SetViewport(static_cast<float>(extent.width),
                                   static_cast<float>(extent.height));
        pCommandBuffer->SetScissor(extent.width, extent.height);
        pCommandBuffer->BindVertexBuffers(*m_vertexBuffer);
        pCommandBuffer->BindIndexBuffer(*m_indexBuffer, VK_INDEX_TYPE_UINT32);
        pCommandBuffer->DrawIndices(m_indexBuffer->GetIndexCount(), 1);
    });
    m_renderGraph->SetBackBufferTag("Output");
    m_renderGraph->SetBackBufferSize(m_window->GetExtent2D().width, m_window->GetExtent2D().height);

    m_renderGraph->Compile();
}

void HelloTriangle::Run()
{
    while (!m_window->ShouldClose())
    {
        m_window->Update();
        Update(m_timer->Tick());
        auto* pCommandBuffer = m_renderContext->StartFrame(val::CommandPool::ResetMode::ResetPool);
        m_renderContext->UpdateUniformBuffer<CameraUniformData>(
            &m_cameraUniformData, m_cameraUniformBuffer.Get(), commandBuffer);
        m_renderGraph->Execute(pCommandBuffer, m_renderContext.Get());
        m_renderContext->EndFrame();
    }
}

void HelloTriangle::LoadTexture()
{
    m_pSimpleTexture = m_textureManager->RequestTexture2D("wood.png");
    m_sampler       = MakeUnique<val::Sampler>(*m_device);
}

void HelloTriangle::LoadModel()
{
    std::vector<Vertex> vertices = {
        {Vec3(0.5f, 0.5f, 1.0f), Vec3(1.0f, 0.0f, 0.0f), Vec2(0.0f, 1.0f)},
        {Vec3(0.5f, -0.5f, 1.0f), Vec3(0.0f, 1.0f, 0.0f), Vec2(1.0f, 1.0f)},
        {Vec3(-0.5f, 0.5f, 1.0f), Vec3(0.0f, 0.0f, 1.0f), Vec2(0.0f, 0.0f)}};
    std::vector<uint32_t> indices = {0, 1, 2};

    m_vertexBuffer = VertexBuffer::CreateUnique(*m_device, GetArrayViewSize(MakeView(vertices)));
    m_indexBuffer  = IndexBuffer::CreateUnique(*m_device, MakeView(indices));

    auto* pStagingBuffer     = m_renderContext->GetCurrentStagingBuffer();
    auto verticesSubmitInfo = pStagingBuffer->Submit(MakeView(vertices));
    auto indicesSubmitInfo  = pStagingBuffer->Submit(MakeView(indices));
    auto* pCmdBuffer         = m_renderContext->GetCommandBuffer();
    pCmdBuffer->Begin();
    pCmdBuffer->CopyBuffer(pStagingBuffer, verticesSubmitInfo.offset, m_vertexBuffer.Get(), 0,
                          verticesSubmitInfo.size);
    pCmdBuffer->CopyBuffer(pStagingBuffer, indicesSubmitInfo.offset, m_indexBuffer.Get(), 0,
                          indicesSubmitInfo.size);
    pCmdBuffer->End();
    m_renderContext->SubmitImmediate(pCmdBuffer);
    m_renderContext->ResetCommandPool();
}
} // namespace zen
