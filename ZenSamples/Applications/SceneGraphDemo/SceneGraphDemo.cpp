#include "SceneGraphDemo.h"
#include "SceneGraph/Scene.h"
#include "Graphics/RenderCore/RenderConfig.h"

namespace zen
{
SceneGraphDemo::SceneGraphDemo()
{
    // set global render config
    RenderConfig& renderConfig          = RenderConfig::GetInstance();
    renderConfig.useSecondaryCmd        = true;
    renderConfig.numThreads             = 4;
    renderConfig.numSecondaryCmdBuffers = 4;

    m_threadPool = MakeUnique<ThreadPool<void, uint32_t>>(renderConfig.numThreads);
    LOGI("Using {} threads for rendering...", renderConfig.numThreads)
}

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

    m_timer = MakeUnique<platform::Timer>();
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
    m_cameraUniformData.cameraPos = Vec4(m_camera->GetPos(), 1.0f);
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
    mainPass->ReadFromExternalBuffer("lightUniform", m_lightUBO.Get());

    mainPass->BindSRD("cameraUniform", VK_SHADER_STAGE_VERTEX_BIT, "uCameraData");
    mainPass->BindSRD("materialUniform", VK_SHADER_STAGE_FRAGMENT_BIT, "uMaterialData");
    mainPass->BindSRD("nodeUniform", VK_SHADER_STAGE_VERTEX_BIT, "uNodeData");
    mainPass->BindSRD("lightUniform", VK_SHADER_STAGE_FRAGMENT_BIT, "uLightsData");
    mainPass->BindSRD("textures", VK_SHADER_STAGE_FRAGMENT_BIT, "uTextureArray");
    mainPass->BindSampler("textures", m_sampler.Get());

    m_renderGraph->SetBackBufferTag("Output");
    m_renderGraph->SetBackBufferSize(m_window->GetExtent2D().width, m_window->GetExtent2D().height);
    m_renderGraph->Compile();

    // set execute after compile
    m_renderGraph->SetOnExecute("MainPass", [&](val::CommandBuffer* commandBuffer) {
        auto& physicalPass =
            m_renderGraph->GetPhysicalPass(m_renderGraph->GetRDGPass("MainPass")->GetIndex());
        if (RenderConfig::GetInstance().useSecondaryCmd)
        {
            RecordDrawCmdsSecondary(commandBuffer, physicalPass);
        }
        else
        {
            RecordDrawCmdsPrimary(commandBuffer, m_scene->GetRenderableNodes(), physicalPass);
        }
    });
}

void SceneGraphDemo::RecordDrawCmdsSecondary(val::CommandBuffer* primaryCmdBuffer,
                                             const RDGPhysicalPass& physicalPass)
{
    auto subMeshes =
        m_scene->GetSortedSubMeshes(m_camera->GetPos(), m_cameraUniformData.modelMatrix);

    uint32_t num2ndCmdBuffers = RenderConfig::GetInstance().numSecondaryCmdBuffers;
    uint32_t numMeshes        = util::ToU32(subMeshes.size());
    uint32_t numDrawPerCmd    = numMeshes / num2ndCmdBuffers;
    uint32_t numDrawRemained  = numMeshes % num2ndCmdBuffers;
    uint32_t meshStart        = 0;

    std::vector<std::future<val::CommandBuffer*>> secondaryCmdFutures;
    std::vector<val::CommandBuffer*> secondaryCmds;
    for (auto i = 0u; i < num2ndCmdBuffers; i++)
    {
        uint32_t meshEnd = std::min(numMeshes, meshStart + numDrawPerCmd);
        if (numDrawRemained > 0)
        {
            meshEnd++;
            numDrawRemained--;
        }
        if (RenderConfig::GetInstance().numThreads > 1)
        {
            auto fut = m_threadPool->Push([this, &primaryCmdBuffer, meshStart, meshEnd, &subMeshes,
                                           &physicalPass](uint32_t threadId) {
                return RecordDrawCmdsSecondary(primaryCmdBuffer, meshStart, meshEnd, subMeshes,
                                               physicalPass, threadId);
            });

            secondaryCmdFutures.push_back(std::move(fut));
        }
        else
        {
            secondaryCmds.push_back(RecordDrawCmdsSecondary(primaryCmdBuffer, meshStart, meshEnd,
                                                            subMeshes, physicalPass));
        }

        meshStart = meshEnd;
    }
    if (RenderConfig::GetInstance().numThreads > 1)
    {
        for (auto& fut : secondaryCmdFutures)
        {
            secondaryCmds.push_back(fut.get());
        }
    }

    primaryCmdBuffer->ExecuteCommands(secondaryCmds);
}

val::CommandBuffer* SceneGraphDemo::RecordDrawCmdsSecondary(
    val::CommandBuffer* primaryCmdBuffer,
    uint32_t meshStart,
    uint32_t meshEnd,
    // all sub meshes and their nodes
    const std::vector<std::pair<sg::Node*, sg::SubMesh*>>& subMeshes,
    // related scene graph physical pass
    const RDGPhysicalPass& physicalPass,
    // thread id for command buffer
    uint32_t threadId)
{
    auto* secondaryCmdBuffer = m_renderContext->GetActiveFrame().RequestCommandBuffer(
        m_device->GetQueue(val::QUEUE_INDEX_GRAPHICS).GetFamilyIndex(),
        val::CommandPool::ResetMode::ResetPool, VK_COMMAND_BUFFER_LEVEL_SECONDARY, threadId);

    secondaryCmdBuffer->Begin(primaryCmdBuffer->GetInheritanceInfo(),
                              VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT |
                                  VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT);
    auto extent = m_renderContext->GetSwapchainExtent2D();
    secondaryCmdBuffer->SetViewport(static_cast<float>(extent.width),
                                    static_cast<float>(extent.height));
    secondaryCmdBuffer->SetScissor(extent.width, extent.height);

    secondaryCmdBuffer->BindVertexBuffers(*m_vertexBuffer);
    secondaryCmdBuffer->BindIndexBuffer(*m_indexBuffer, VK_INDEX_TYPE_UINT32);

    if (physicalPass.graphicPipeline)
    {
        secondaryCmdBuffer->BindGraphicPipeline(physicalPass.graphicPipeline->GetHandle());
    }
    if (!physicalPass.descriptorSets.empty())
    {
        secondaryCmdBuffer->BindDescriptorSets(physicalPass.pipelineLayout->GetHandle(),
                                               physicalPass.descriptorSets);
    }
    PushConstantsData pushConstantData = m_pushConstantData;
    for (auto i = meshStart; i < meshEnd; i++)
    {
        auto* node    = subMeshes[i].first;
        auto* subMesh = subMeshes[i].second;

        pushConstantData.nodeIndex     = m_nodesUniformIndex[node->GetHash()];
        pushConstantData.materialIndex = subMesh->GetMaterial()->index;
        secondaryCmdBuffer->PushConstants(physicalPass.pipelineLayout->GetHandle(),
                                          VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                          &pushConstantData);
        secondaryCmdBuffer->DrawIndices(subMesh->GetIndexCount(), 1, subMesh->GetFirstIndex());
    }
    secondaryCmdBuffer->End();
    return secondaryCmdBuffer;
}

void SceneGraphDemo::RecordDrawCmdsPrimary(val::CommandBuffer* primaryCmdBuffer,
                                           const std::vector<sg::Node*>& nodes,
                                           const RDGPhysicalPass& physicalPass)
{
    primaryCmdBuffer->BindVertexBuffers(*m_vertexBuffer);
    primaryCmdBuffer->BindIndexBuffer(*m_indexBuffer, VK_INDEX_TYPE_UINT32);
    auto extent = m_renderContext->GetSwapchainExtent2D();
    primaryCmdBuffer->SetViewport(static_cast<float>(extent.width),
                                  static_cast<float>(extent.height));
    primaryCmdBuffer->SetScissor(extent.width, extent.height);
    if (physicalPass.graphicPipeline)
    {
        primaryCmdBuffer->BindGraphicPipeline(physicalPass.graphicPipeline->GetHandle());
    }
    if (!physicalPass.descriptorSets.empty())
    {
        primaryCmdBuffer->BindDescriptorSets(physicalPass.pipelineLayout->GetHandle(),
                                             physicalPass.descriptorSets);
    }
    for (auto* node : nodes)
    {
        m_pushConstantData.nodeIndex = m_nodesUniformIndex[node->GetHash()];
        for (auto* subMesh : node->GetComponent<sg::Mesh>()->GetSubMeshes())
        {
            m_pushConstantData.materialIndex = subMesh->GetMaterial()->index;
            primaryCmdBuffer->PushConstants(
                physicalPass.pipelineLayout->GetHandle(),
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, &m_pushConstantData);
            primaryCmdBuffer->DrawIndices(subMesh->GetIndexCount(), 1, subMesh->GetFirstIndex());
        }
    }
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
    gltfLoader->LoadFromFile(GLTF_PATHS[3], m_scene.Get());

    TransformScene();

    AddStaticLights();

    FillNodeUniforms();

    FillTextureArray();

    FillMaterialUniforms();

    FillLightUniforms();

    // configure camera
    m_camera = sg::Camera::CreateUniqueOnAABB(m_scene->GetAABB().GetMin(),
                                               m_scene->GetAABB().GetMax(), m_window->GetAspect());

    const auto& modelVertices = gltfLoader->GetVertices();
    const auto& modelIndices  = gltfLoader->GetIndices();

    m_vertexBuffer = VertexBuffer::CreateUnique(
        *m_device, GetArrayViewSize(MakeView(gltfLoader->GetVertices())));
    m_indexBuffer = IndexBuffer::CreateUnique(*m_device, MakeView(gltfLoader->GetIndices()));

    auto* stagingBuffer     = m_renderContext->GetCurrentStagingBuffer();
    auto verticesSubmitInfo = stagingBuffer->Submit(MakeView(modelVertices));
    auto indicesSubmitInfo  = stagingBuffer->Submit(MakeView(modelIndices));
    auto* cmdBuffer         = m_renderContext->GetCommandBuffer();
    cmdBuffer->Begin();
    cmdBuffer->CopyBuffer(stagingBuffer, verticesSubmitInfo.offset, m_vertexBuffer.Get(), 0,
                          verticesSubmitInfo.size);
    cmdBuffer->CopyBuffer(stagingBuffer, indicesSubmitInfo.offset, m_indexBuffer.Get(), 0,
                          indicesSubmitInfo.size);
    m_renderContext->UpdateRenderBuffer(MakeView(m_nodesUniforms), m_nodesUBO.Get(), cmdBuffer);
    // upload material uniforms
    m_renderContext->UpdateRenderBuffer<MaterialUniformData>(MakeView(m_materialUniforms),
                                                             m_materialUBO.Get(), cmdBuffer);
    // upload static light uniforms
    m_renderContext->UpdateRenderBuffer<LightUniformData>(MakeView(m_lightUniforms),
                                                          m_lightUBO.Get(), cmdBuffer);

    cmdBuffer->End();
    m_renderContext->SubmitImmediate(cmdBuffer);
    m_renderContext->ResetCommandPool();
}

void SceneGraphDemo::TransformScene()
{
    // Center and scale model
    Vec3 center(0.0f);
    Vec3 extents = m_scene->GetAABB().GetMax() - m_scene->GetAABB().GetMin();
    Vec3 scaleFactors(1.0f);
    scaleFactors.x = glm::abs(extents.x) < glm::epsilon<float>() ? 1.0f : 1.0f / extents.x;
    scaleFactors.y = glm::abs(extents.y) < glm::epsilon<float>() ? 1.0f : 1.0f / extents.y;
    scaleFactors.z = glm::abs(extents.z) < glm::epsilon<float>() ? 1.0f : 1.0f / extents.z;

    auto scaleFactorMax = std::max(scaleFactors.x, std::max(scaleFactors.y, scaleFactors.z));
    Mat4 scaleMat       = glm::scale(Mat4(1.0f), Vec3(scaleFactorMax));
    Mat4 translateMat   = glm::translate(Mat4(1.0f), center - m_scene->GetAABB().GetCenter());

    m_cameraUniformData.modelMatrix = scaleMat * translateMat;

    m_scene->GetAABB().Transform(m_cameraUniformData.modelMatrix);
}

void SceneGraphDemo::AddStaticLights()
{
    auto pos0 = m_scene->GetAABB().GetMax();
    auto pos1 = Vec3(-pos0.x, pos0.y, pos0.z);
    auto pos2 = Vec3(pos0.x, pos0.y, -pos0.z);
    auto pos3 = Vec3(-pos0.x, pos0.y, -pos0.z);

    sg::LightProperties props0{pos0, Vec4(1.0f), Vec4(-1.0f)};
    sg::LightProperties props1{pos1, Vec4(1.0f), Vec4(-1.0f)};
    sg::LightProperties props2{pos2, Vec4(1.0f), Vec4(-1.0f)};
    sg::LightProperties props3{pos3, Vec4(1.0f), Vec4(-1.0f)};
    m_scene->AddComponent(sg::Light::CreatePointLight("light0", props0));
    m_scene->AddComponent(sg::Light::CreatePointLight("light1", props1));
    m_scene->AddComponent(sg::Light::CreatePointLight("light2", props2));
    m_scene->AddComponent(sg::Light::CreatePointLight("light3", props3));
}

void SceneGraphDemo::FillTextureArray()
{
    m_textureManager->RegisterSceneTextures(m_scene.Get());
    for (const auto* tex : m_scene->GetComponents<sg::Texture>())
    {
        m_textureArray.push_back(m_textureManager->RequestTexture2D(tex->GetName()));
    }
}

void SceneGraphDemo::FillLightUniforms()
{
    auto sgLights = m_scene->GetComponents<sg::Light>();
    for (const auto* sgLight : sgLights)
    {
        const auto& props = sgLight->GetProperties();
        m_lightUniforms.push_back(
            {Vec4(props.position, sgLight->GetType()), props.color, props.direction});
    }
    auto uboSize =
        m_renderDevice->PadUniformBufferSize(sizeof(LightUniformData)) * m_lightUniforms.size();
    m_lightUBO = UniformBuffer::CreateUnique(*m_device, uboSize);
    m_lightUBO->SetObjectDebugName("LightUniformBuffer");
    m_pushConstantData.numLights = m_lightUniforms.size();
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
        if (mat->baseColorTexture != nullptr)
        {
            matUniformData.bcTexIndex = mat->baseColorTexture->index;
            matUniformData.bcTexSet   = mat->texCoordSets.baseColor;
        }
        if (mat->metallicRoughnessTexture != nullptr)
        {
            matUniformData.mrTexIndex = mat->metallicRoughnessTexture->index;
            matUniformData.mrTexSet   = mat->texCoordSets.metallicRoughness;
        }
        if (mat->normalTexture != nullptr)
        {
            matUniformData.normalTexIndex = mat->normalTexture->index;
            matUniformData.normalTexSet   = mat->texCoordSets.normal;
        }
        if (mat->occlusionTexture != nullptr)
        {
            matUniformData.aoTexIndex = mat->occlusionTexture->index;
            matUniformData.aoTexSet   = mat->texCoordSets.occlusion;
        }
        if (mat->emissiveTexture != nullptr)
        {
            matUniformData.emissiveTexIndex = mat->emissiveTexture->index;
            matUniformData.emissiveTexSet   = mat->texCoordSets.emissive;
        }
        matUniformData.metallicFactor  = mat->metallicFactor;
        matUniformData.roughnessFactor = mat->roughnessFactor;
        matUniformData.emissiveFactor  = mat->emissiveFactor;
        m_materialUniforms.emplace_back(matUniformData);
    }

    const auto uboSize = m_renderDevice->PadUniformBufferSize(sizeof(MaterialUniformData)) *
        m_materialUniforms.size();
    m_materialUBO = UniformBuffer::CreateUnique(*m_device, uboSize);
    m_materialUBO->SetObjectDebugName("MaterialSSBO");
}
} // namespace zen
