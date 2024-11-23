#include <random>
#include "PushConstants.h"
#include "AssetLib/GLTFLoader.h"

PushConstantsApp::PushConstantsApp(const platform::WindowConfig& windowConfig) :
    Application(windowConfig, zen::sys::CameraType::eOrbit)
{
    m_camera->SetPosition({0.0f, 0.0f, -5.0f});
}

void PushConstantsApp::Prepare()
{
    Application::Prepare();
    LoadResources();
    BuildRenderPipeline();
    BuildRenderGraph();
}

void PushConstantsApp::Run()
{
    Application::Run();
    while (!m_window->ShouldClose())
    {
        auto frameTime = static_cast<float>(m_timer->Tick());

        m_window->Update();
        m_camera->Update(frameTime);
        m_renderDevice->ExecuteFrame(m_viewport, m_rdg.Get());
        m_renderDevice->NextFrame();
    }
}

void PushConstantsApp::Destroy()
{
    Application::Destroy();
}

void PushConstantsApp::BuildRenderPipeline()
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
        binding1.handles.push_back(m_modelUBO);
        uboBindings.emplace_back(std::move(binding1));
    }

    rc::RenderPipelineBuilder builder(m_renderDevice);
    m_mainRP =
        builder.SetVertexShader("push_constants.vert.spv")
            .SetFragmentShader("push_constants.frag.spv")
            .SetNumSamples(SampleCount::e1)
            .AddColorRenderTarget(m_viewport->GetSwapchainFormat(), TextureUsage::eColorAttachment)
            .SetDepthStencilTarget(m_viewport->GetDefaultDepthStencilFormat())
            .SetShaderResourceBinding(0, uboBindings)
            .SetPipelineState(pso)
            .Build();
}

void PushConstantsApp::LoadResources()
{
    Application::LoadResources();
    // Setup random colors and fixed positions for every sphere in the scene
    std::random_device rndDevice;
    std::default_random_engine rndEngine(rndDevice());
    std::uniform_real_distribution<float> rndDist(0.1f, 1.0f);
    for (uint32_t i = 0; i < m_boxPCs.size(); i++)
    {
        m_boxPCs[i].color = Vec4(rndDist(rndEngine), rndDist(rndEngine), rndDist(rndEngine), 1.0f);
        const float rad   = glm::radians(i * 360.0f / static_cast<uint32_t>(m_boxPCs.size()));
        m_boxPCs[i].position = Vec4(glm::vec3(sin(rad), cos(rad), 0.0f) * 3.5f, 1.0f);
    }

    m_scene         = MakeUnique<sg::Scene>();
    auto gltfLoader = MakeUnique<gltf::GltfLoader>();
    gltfLoader->LoadFromFile(m_modelPath, m_scene.Get());

    uint32_t vbSize = gltfLoader->GetVertices().size() * sizeof(gltf::Vertex);
    m_vertexBuffer  = m_renderDevice->CreateVertexBuffer(
        vbSize, reinterpret_cast<const uint8_t*>(gltfLoader->GetVertices().data()));

    m_indexBuffer = m_renderDevice->CreateIndexBuffer(
        gltfLoader->GetIndices().size() * sizeof(uint32_t),
        reinterpret_cast<const uint8_t*>(gltfLoader->GetIndices().data()));

    m_modelData.model = glm::scale(Mat4(1.0f), Vec3(0.5f));
    m_modelUBO        = m_renderDevice->CreateUniformBuffer(
        sizeof(ModelData), reinterpret_cast<const uint8_t*>(&m_modelData));
}

void PushConstantsApp::BuildRenderGraph()
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


    FramebufferHandle framebuffer = m_viewport->GetCompatibleFramebuffer(m_mainRP.renderPass);
    auto* mainPass =
        m_rdg->AddGraphicsPassNode(m_mainRP.renderPass, framebuffer, area, clearValues, true);
    m_rdg->AddGraphicsPassBindPipelineNode(mainPass, m_mainRP.pipeline, PipelineType::eGraphics);
    m_rdg->AddGraphicsPassBindVertexBufferNode(mainPass, m_vertexBuffer, {0});
    m_rdg->AddGraphicsPassBindIndexBufferNode(mainPass, m_indexBuffer, DataFormat::eR32UInt);
    m_rdg->AddGraphicsPassSetViewportNode(mainPass, vp);
    m_rdg->AddGraphicsPassSetScissorNode(mainPass, area);
    for (auto i = 0; i < NUM_BOXES; i++)
    {
        m_rdg->AddGraphicsPassSetPushConstants(mainPass, &m_boxPCs[i], sizeof(PushConstantData));
        for (auto* node : m_scene->GetRenderableNodes())
        {
            for (auto* subMesh : node->GetComponent<sg::Mesh>()->GetSubMeshes())
            {
                m_rdg->AddGraphicsPassDrawIndexedNode(mainPass, subMesh->GetIndexCount(), 1,
                                                      subMesh->GetFirstIndex(), 0, 0);
            }
        }
    }
    m_rdg->End();
}


int main(int argc, char** argv)
{
    platform::WindowConfig windowConfig{"push_constants", true, 1280, 720};
    Application* app = new PushConstantsApp(windowConfig);

    app->Prepare();

    app->Run();

    app->Destroy();

    delete app;
}