#include "SceneGraphDemo.h"
#include "SceneGraph/Scene.h"

namespace zen
{
SceneGraphDemo::SceneGraphDemo() {}

void SceneGraphDemo::Prepare(const platform::WindowConfig& windowConfig)
{
    Application::Prepare(windowConfig);
    // set up render device
    m_renderDevice  = MakeUnique<RenderDevice>(*m_device);
    m_renderContext = MakeUnique<RenderContext>(*m_device, m_window.Get());
    m_renderGraph   = MakeUnique<RenderGraph>(*m_renderDevice);
    m_shaderManager = MakeUnique<ShaderManager>(*m_device);

    m_textureManager = MakeUnique<TextureManager>(*m_device, *m_renderContext);

    m_cameraUniformBuffer = UniformBuffer::CreateUnique(*m_device, sizeof(CameraUniformData));


    m_camera = sys::Camera::CreateUnique(Vec3{0.0f, 0.0f, -0.1f}, Vec3{0.0f, 0.0f, 0.0f},
                                         m_window->GetAspect());
    m_timer  = MakeUnique<platform::Timer>();
    m_window->SetOnResize([&](uint32_t width, uint32_t height) {
        m_renderContext->RecreateSwapchain(width, height);
        m_renderGraph = MakeUnique<RenderGraph>(*m_renderDevice);
        SetupRenderGraph();
    });

    LoadTexture();

    LoadScene();

    SetupRenderGraph();
}

void SceneGraphDemo::Update(float deltaTime)
{
    Application::Update(deltaTime);
    m_camera->Update(deltaTime);
    m_cameraUniformData.projViewMatrix =
        m_camera->GetProjectionMatrix() * m_camera->GetViewMatrix();
}

void SceneGraphDemo::SetupRenderGraph()
{
    val::ShaderModule* vertexShader =
        m_shaderManager->RequestShader("viewer.vert.spv", VK_SHADER_STAGE_VERTEX_BIT, {});
    val::ShaderModule* fragShader =
        m_shaderManager->RequestShader("viewer.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT, {});

    RDGPass* mainPass = m_renderGraph->AddPass("MainPass", RDG_QUEUE_GRAPHICS_BIT);
    mainPass->UseShaders({vertexShader, fragShader});
    RDGImage::Info outputImgInfo{};
    outputImgInfo.format = m_renderContext->GetSwapchainFormat();
    mainPass->WriteToColorImage("Output", outputImgInfo);
    mainPass->ReadFromExternalImage("texture", m_simpleTexture);
    mainPass->ReadFromExternalBuffer("cameraUniform", m_cameraUniformBuffer.Get());
    mainPass->ReadFromExternalBuffer("nodeUniform", m_nodesUniformBuffer.Get());

    mainPass->BindSRD("cameraUniform", VK_SHADER_STAGE_VERTEX_BIT, "uCameraData");
    mainPass->BindSRD("nodeUniform", VK_SHADER_STAGE_VERTEX_BIT, "uNodeData");
    mainPass->BindSRD("texture", VK_SHADER_STAGE_FRAGMENT_BIT, "uTexture");
    mainPass->BindSampler("texture", m_sampler.Get());

    m_renderGraph->SetBackBufferTag("Output");
    m_renderGraph->SetBackBufferSize(m_window->GetExtent2D().width, m_window->GetExtent2D().height);
    m_renderGraph->Compile();

    // set execute after compile
    m_renderGraph->SetOnExecute("MainPass", [&](val::CommandBuffer* commandBuffer) {
        auto extent = m_renderContext->GetSwapchainExtent2D();
        commandBuffer->SetViewport(static_cast<float>(extent.width),
                                   static_cast<float>(extent.height));
        commandBuffer->SetScissor(extent.width, extent.height);
        commandBuffer->BindVertexBuffers(*m_vertexBuffer);
        commandBuffer->BindIndexBuffer(*m_indexBuffer, VK_INDEX_TYPE_UINT32);

        auto& physicalPass =
            m_renderGraph->GetPhysicalPass(m_renderGraph->GetRDGPass("MainPass")->GetIndex());
        for (auto* node : m_scene->GetRenderableNodes())
        {
            auto index = m_nodesUniformIndex[node];
            commandBuffer->PushConstants(physicalPass.pipelineLayout->GetHandle(),
                                         VK_SHADER_STAGE_VERTEX_BIT, &index);
            for (auto* subMesh : node->GetComponent<sg::Mesh>()->GetSubMeshes())
            {
                commandBuffer->DrawIndices(subMesh->GetIndexCount(), 1, subMesh->GetFirstIndex());
            }
        }
    });
}

void SceneGraphDemo::Run()
{
    while (!m_window->ShouldClose())
    {
        m_window->Update();
        Update(m_timer->Tick());
        auto* commandBuffer = m_renderContext->StartFrame(val::CommandPool::ResetMode::ResetPool);
        m_renderContext->UpdateUniformBuffer<CameraUniformData>(
            &m_cameraUniformData, m_cameraUniformBuffer.Get(), commandBuffer);
        m_renderGraph->Execute(commandBuffer, m_renderContext.Get());
        m_renderContext->EndFrame();
    }
}

void SceneGraphDemo::LoadTexture()
{
    m_simpleTexture = m_textureManager->RequestTexture2D("wood.png");
    m_sampler       = MakeUnique<val::Sampler>(*m_device);
}

void SceneGraphDemo::LoadScene()
{
    m_scene         = MakeUnique<sg::Scene>();
    auto gltfLoader = MakeUnique<gltf::GltfLoader>();
    gltfLoader->LoadFromFile(GLTF_PATHS[1], m_scene.Get());

    const auto sceneSize = m_scene->GetSize();
    m_camera->SetFarPlane(sceneSize * 100.0f);
    m_camera->SetSpeed(sceneSize);

    auto& renderableNodes = m_scene->GetRenderableNodes();

    auto index = 0u;
    for (auto* node : renderableNodes)
    {
        m_nodesUniformData.push_back({node->GetComponent<sg::Transform>()->GetWorldMatrix()});
        m_nodesUniformIndex[node] = index;
        index++;
    }

    m_nodesUniformBuffer =
        UniformBuffer::CreateUnique(*m_device, sizeof(NodeUniformData) * renderableNodes.size());
    const auto& modelVertices = gltfLoader->GetVertices();
    const auto& modelIndices  = gltfLoader->GetIndices();

    m_vertexBuffer = VertexBuffer::CreateUnique(
        *m_device, GetArrayViewSize(MakeView(gltfLoader->GetVertices())));
    m_indexBuffer = IndexBuffer::CreateUnique(*m_device, MakeView(gltfLoader->GetIndices()));

    auto* stagingBuffer      = m_renderContext->GetCurrentStagingBuffer();
    auto  verticesSubmitInfo = stagingBuffer->Submit(MakeView(modelVertices));
    auto  indicesSubmitInfo  = stagingBuffer->Submit(MakeView(modelIndices));
    auto* cmdBuffer          = m_renderContext->GetCommandBuffer();
    cmdBuffer->Begin();
    cmdBuffer->CopyBuffer(stagingBuffer, verticesSubmitInfo.offset, m_vertexBuffer.Get(), 0,
                          verticesSubmitInfo.size);
    cmdBuffer->CopyBuffer(stagingBuffer, indicesSubmitInfo.offset, m_indexBuffer.Get(), 0,
                          indicesSubmitInfo.size);
    m_renderContext->UpdateUniformBuffer(MakeView(m_nodesUniformData), m_nodesUniformBuffer.Get(),
                                         cmdBuffer);
    cmdBuffer->End();
    m_renderContext->SubmitImmediate(cmdBuffer);
    m_renderContext->ResetCommandPool();
}
} // namespace zen
