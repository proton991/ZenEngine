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

    m_cameraUBO = UniformBuffer::CreateUnique(*m_device, sizeof(CameraUniformData));
    m_cameraUBO->SetObjectDebugName("CameraUniformBuffer");

    m_camera = sys::Camera::CreateUnique(Vec3{0.0f, 0.0f, -0.1f}, Vec3{0.0f, 0.0f, 0.0f},
                                         m_window->GetAspect());
    m_timer  = MakeUnique<platform::Timer>();
    m_window->SetOnResize([&](uint32_t width, uint32_t height) {
        m_renderContext->RecreateSwapchain(width, height);
        m_renderGraph = MakeUnique<RenderGraph>(*m_renderDevice);
        SetupRenderGraph();
    });

    m_sampler = MakeUnique<val::Sampler>(*m_device);

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
        m_shaderManager->RequestShader("main.vert.spv", VK_SHADER_STAGE_VERTEX_BIT, {});
    val::ShaderModule* fragShader =
        m_shaderManager->RequestShader("pbr.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT, {});

    RDGPass* mainPass = m_renderGraph->AddPass("MainPass", RDG_QUEUE_GRAPHICS_BIT);
    mainPass->UseShaders({vertexShader, fragShader});
    RDGImage::Info outputImgInfo{};
    outputImgInfo.format = m_renderContext->GetSwapchainFormat();
    RDGImage::Info outputDepthInfo{};
    outputDepthInfo.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    mainPass->WriteToColorImage("Output", outputImgInfo);
    mainPass->WriteToDepthStencilImage("OutputDS", outputDepthInfo);
    mainPass->ReadFromExternalImages("textures", m_textureArray);
    mainPass->ReadFromExternalBuffer("cameraUniform", m_cameraUBO.Get());
    mainPass->ReadFromExternalBuffer("nodeUniform", m_nodesUBO.Get());
    mainPass->ReadFromExternalBuffer("materialUniform", m_materialUBO.Get());

    mainPass->BindSRD("cameraUniform", VK_SHADER_STAGE_VERTEX_BIT, "uCameraData");
    mainPass->BindSRD("materialUniform", VK_SHADER_STAGE_FRAGMENT_BIT, "uMaterialData");
    mainPass->BindSRD("nodeUniform", VK_SHADER_STAGE_VERTEX_BIT, "uNodeData");
    mainPass->BindSRD("textures", VK_SHADER_STAGE_FRAGMENT_BIT, "uTextureArray");
    mainPass->BindSampler("textures", m_sampler.Get());

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
            m_pushConstantData.nodeIndex = m_nodesUniformIndex[node->GetHash()];
            for (auto* subMesh : node->GetComponent<sg::Mesh>()->GetSubMeshes())
            {
                m_pushConstantData.materialIndex = subMesh->GetMaterial()->index;
                commandBuffer->PushConstants(
                    physicalPass.pipelineLayout->GetHandle(),
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, &m_pushConstantData);
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
        m_renderContext->UpdateRenderBuffer<CameraUniformData>(&m_cameraUniformData,
                                                               m_cameraUBO.Get(), commandBuffer);
        m_renderGraph->Execute(commandBuffer, m_renderContext.Get());
        m_renderContext->EndFrame();
    }
}

void SceneGraphDemo::LoadScene()
{
    m_scene         = MakeUnique<sg::Scene>();
    auto gltfLoader = MakeUnique<gltf::GltfLoader>();
    gltfLoader->LoadFromFile(GLTF_PATHS[2], m_scene.Get());

    FillNodeUniforms();

    FillTextureArray();

    FillMaterialUniforms();

    // configure camera
    const auto sceneSize = m_scene->GetSize();
    m_camera->SetFarPlane(sceneSize * 10.0f);
    m_camera->SetNearPlane(sceneSize * 0.01f);
    m_camera->SetSpeed(sceneSize);

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
    m_renderContext->UpdateRenderBuffer(MakeView(m_nodesUniforms), m_nodesUBO.Get(), cmdBuffer);
    // upload material uniforms
    m_renderContext->UpdateRenderBuffer<MaterialUniformData>(MakeView(m_materialUniforms),
                                                             m_materialUBO.Get(), cmdBuffer);

    cmdBuffer->End();
    m_renderContext->SubmitImmediate(cmdBuffer);
    m_renderContext->ResetCommandPool();
}

void SceneGraphDemo::FillTextureArray()
{
    m_textureManager->RegisterSceneTextures(m_scene.Get());
    for (const auto* tex : m_scene->GetComponents<sg::Texture>())
    {
        m_textureArray.push_back(m_textureManager->RequestTexture2D(tex->GetName()));
    }
}

void SceneGraphDemo::FillNodeUniforms()
{
    auto& renderableNodes = m_scene->GetRenderableNodes();

    auto index = 0u;
    for (auto* node : renderableNodes)
    {
        NodeUniformData nodeUniformData{};
        nodeUniformData.modelMatrix = node->GetComponent<sg::Transform>()->GetWorldMatrix();
        // pre-calculate normal transform matrix
        nodeUniformData.normalMatrix = glm::transpose(glm::inverse(nodeUniformData.modelMatrix));
        m_nodesUniforms.emplace_back(nodeUniformData);
        m_nodesUniformIndex[node->GetHash()] = index;
        index++;
    }

    auto uboSize =
        m_renderDevice->PadUniformBufferSize(sizeof(NodeUniformData)) * m_nodesUniforms.size();
    m_nodesUBO = UniformBuffer::CreateUnique(*m_device, uboSize);
    m_nodesUBO->SetObjectDebugName("NodesUniformBuffer");
}

void SceneGraphDemo::FillMaterialUniforms()
{
    auto sgMaterials = m_scene->GetComponents<sg::Material>();
    m_materialUniforms.reserve(sgMaterials.size());
    for (const auto* mat : sgMaterials)
    {
        MaterialUniformData matUniformData{};
        matUniformData.bcTexIndex = mat->baseColorTexture ? mat->baseColorTexture->index : -1;
        matUniformData.mrTexIndex =
            mat->metallicRoughnessTexture ? mat->metallicRoughnessTexture->index : -1;
        matUniformData.normalTexIndex   = mat->normalTexture ? mat->normalTexture->index : -1;
        matUniformData.aoTexIndex       = mat->occlusionTexture ? mat->occlusionTexture->index : -1;
        matUniformData.emissiveTexIndex = mat->emissiveTexture ? mat->emissiveTexture->index : -1;
        matUniformData.metallicFactor   = mat->metallicFactor;
        matUniformData.roughnessFactor  = mat->roughnessFactor;
        matUniformData.emissiveFactor   = mat->emissiveFactor;
        m_materialUniforms.emplace_back(matUniformData);
    }

    //    auto originalSize = sizeof(MaterialUniformData);
    //
    const auto uboSize = m_renderDevice->PadUniformBufferSize(sizeof(MaterialUniformData)) *
        m_materialUniforms.size();
    m_materialUBO = UniformBuffer::CreateUnique(*m_device, uboSize);
    m_materialUBO->SetObjectDebugName("MaterialSSBO");
}
} // namespace zen
