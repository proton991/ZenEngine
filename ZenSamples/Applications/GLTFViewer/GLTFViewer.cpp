#include "GLTFViewer.h"

namespace zen
{
GLTFViewer::GLTFViewer() {}

void GLTFViewer::Prepare(const platform::WindowConfig& windowConfig)
{
    Application::Prepare(windowConfig);
    m_gltfModelLoader = MakeUnique<gltf::ModelLoader>();
    // set up render device
    m_renderDevice  = MakeUnique<RenderDevice>(*m_device);
    m_renderContext = MakeUnique<RenderContext>(*m_device, m_window.Get());
    m_renderGraph   = MakeUnique<RenderGraph>(*m_renderDevice);
    m_shaderManager = MakeUnique<ShaderManager>(*m_device);

    m_textureManager = MakeUnique<TextureManager>(*m_device, *m_renderContext);

    m_cameraUniformBuffer = UniformBuffer::CreateUnique(*m_device, sizeof(CameraUniformData));


    m_camera = sg::Camera::CreateUnique(Vec3{0.0f, 0.0f, -0.1f}, Vec3{0.0f, 0.0f, 0.0f},
                                         m_window->GetAspect());
    m_timer  = MakeUnique<platform::Timer>();
    m_window->SetOnResize([&](uint32_t width, uint32_t height) {
        m_renderContext->RecreateSwapchain(width, height);
        m_renderGraph = MakeUnique<RenderGraph>(*m_renderDevice);
        SetupRenderGraph();
    });

    LoadTexture();

    LoadModel();

    SetupRenderGraph();
}

void GLTFViewer::Update(float deltaTime)
{
    Application::Update(deltaTime);
    m_camera->Update(deltaTime);
    m_cameraUniformData.projViewMatrix =
        m_camera->GetProjectionMatrix() * m_camera->GetViewMatrix();
}

void GLTFViewer::SetupRenderGraph()
{
    val::ShaderModule* pVertexShader =
        m_shaderManager->RequestShader("viewer.vert.spv", VK_SHADER_STAGE_VERTEX_BIT, {});
    val::ShaderModule* pFragShader =
        m_shaderManager->RequestShader("viewer.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT, {});

    RDGPass* pMainPass = m_renderGraph->AddPass("MainPass", RDG_QUEUE_GRAPHICS_BIT);
    pMainPass->UseShaders({pVertexShader, pFragShader});
    RDGImage::Info outputImgInfo{};
    outputImgInfo.format = m_renderContext->GetSwapchainFormat();
    pMainPass->WriteToColorImage("Output", outputImgInfo);
    pMainPass->ReadFromExternalImage("texture", m_pSimpleTexture);
    pMainPass->ReadFromExternalBuffer("cameraUniform", m_cameraUniformBuffer.Get());
    pMainPass->ReadFromExternalBuffer("nodeUniform", m_nodesUniformBuffer.Get());

    pMainPass->BindSRD("cameraUniform", VK_SHADER_STAGE_VERTEX_BIT, "uCameraData");
    pMainPass->BindSRD("nodeUniform", VK_SHADER_STAGE_VERTEX_BIT, "uNodeData");
    pMainPass->BindSRD("texture", VK_SHADER_STAGE_FRAGMENT_BIT, "uTexture");
    pMainPass->BindSampler("texture", m_sampler.Get());

    m_renderGraph->SetBackBufferTag("Output");
    m_renderGraph->SetBackBufferSize(m_window->GetExtent2D().width, m_window->GetExtent2D().height);
    m_renderGraph->Compile();

    // set execute after compile
    m_renderGraph->SetOnExecute("MainPass", [&](val::CommandBuffer* pCommandBuffer) {
        auto extent = m_renderContext->GetSwapchainExtent2D();
        pCommandBuffer->SetViewport(static_cast<float>(extent.width),
                                   static_cast<float>(extent.height));
        pCommandBuffer->SetScissor(extent.width, extent.height);
        pCommandBuffer->BindVertexBuffers(*m_vertexBuffer);
        pCommandBuffer->BindIndexBuffer(*m_indexBuffer, VK_INDEX_TYPE_UINT32);

        auto& physicalPass =
            m_renderGraph->GetPhysicalPass(m_renderGraph->GetRDGPass("MainPass")->GetIndex());
        for (auto* node : m_gltfModel.flatNodes)
        {
            commandBuffer->PushConstants(physicalPass.pipelineLayout->GetHandle(),
                                         VK_SHADER_STAGE_VERTEX_BIT, &node->index);
        }
        RenderGLTFModel(pCommandBuffer);
    });
}

void GLTFViewer::RenderGLTFModel(val::CommandBuffer* pCommandBuffer)
{
    for (auto* node : m_gltfModel.flatNodes)
    {
        for (auto* primitive : node->primitives)
        {
            pCommandBuffer->DrawIndices(primitive->indexCount, 1, primitive->firstIndex);
        }
    }
}

void GLTFViewer::Run()
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

void GLTFViewer::LoadTexture()
{
    m_pSimpleTexture = m_textureManager->RequestTexture2D("wood.png");
    m_sampler       = MakeUnique<val::Sampler>(*m_device);
}

void GLTFViewer::LoadModel()
{
    m_gltfModelLoader->LoadFromFile(MODEL_PATHS[1], &m_gltfModel);
    for (auto* node : m_gltfModel.flatNodes)
    {
        m_nodesUniformData.push_back({node->GetMatrix()});
    }
    m_nodesUniformBuffer = UniformBuffer::CreateUnique(
        *m_device, sizeof(NodeUniformData) * m_gltfModel.flatNodes.size());

    const auto modelSize = m_gltfModel.GetSize();
    m_camera->SetFarPlane(modelSize * 100.0f);
    m_camera->SetSpeed(modelSize);

    const auto& modelVertices = m_gltfModelLoader->GetVertices();
    const auto& modelIndices  = m_gltfModelLoader->GetIndices();

    m_vertexBuffer = VertexBuffer::CreateUnique(
        *m_device, GetArrayViewSize(MakeView(m_gltfModelLoader->GetVertices())));
    m_indexBuffer = IndexBuffer::CreateUnique(*m_device, MakeView(m_gltfModelLoader->GetIndices()));

    auto* pStagingBuffer     = m_renderContext->GetCurrentStagingBuffer();
    auto verticesSubmitInfo = pStagingBuffer->Submit(MakeView(modelVertices));
    auto indicesSubmitInfo  = pStagingBuffer->Submit(MakeView(modelIndices));
    auto* pCmdBuffer         = m_renderContext->GetCommandBuffer();
    pCmdBuffer->Begin();
    pCmdBuffer->CopyBuffer(pStagingBuffer, verticesSubmitInfo.offset, m_vertexBuffer.Get(), 0,
                          verticesSubmitInfo.size);
    pCmdBuffer->CopyBuffer(pStagingBuffer, indicesSubmitInfo.offset, m_indexBuffer.Get(), 0,
                          indicesSubmitInfo.size);
    m_renderContext->UpdateUniformBuffer(MakeView(m_nodesUniformData), m_nodesUniformBuffer.Get(),
                                         pCmdBuffer);
    pCmdBuffer->End();
    m_renderContext->SubmitImmediate(pCmdBuffer);
    m_renderContext->ResetCommandPool();
}
} // namespace zen
