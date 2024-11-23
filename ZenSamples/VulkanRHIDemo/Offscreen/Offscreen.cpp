#include "Offscreen.h"
#include "AssetLib/GLTFLoader.h"

#define OFFSCREEN_TEXTURE_DIM 512

OffscreenApp::OffscreenApp(const platform::WindowConfig& windowConfig) :
    Application(windowConfig, sys::CameraType::eFirstPerson)
{
    m_camera->SetPosition({1.0f, 1.0f, -2.0f});
}

void OffscreenApp::Prepare()
{
    Application::Prepare();
    LoadResources();
    PrepareOffscreenTextures();
    BuildRenderPipeline();
    BuildRenderGraph();
}

void OffscreenApp::Run()
{
    while (!m_window->ShouldClose())
    {
        auto frameTime = static_cast<float>(m_timer->Tick());
        m_window->Update();
        m_camera->Update(frameTime);
        m_renderDevice->ExecuteFrame(m_viewport, m_rdg.Get());
        m_renderDevice->NextFrame();
    }
}

void OffscreenApp::Destroy()
{
    Application::Destroy();
}

void OffscreenApp::PrepareOffscreenTextures()
{
    // offscreen color
    {
        rhi::TextureInfo textureInfo{};
        textureInfo.format      = rhi::DataFormat::eR8G8B8A8SRGB;
        textureInfo.type        = rhi::TextureType::e2D;
        textureInfo.width       = OFFSCREEN_TEXTURE_DIM;
        textureInfo.height      = OFFSCREEN_TEXTURE_DIM;
        textureInfo.depth       = 1;
        textureInfo.arrayLayers = 1;
        textureInfo.mipmaps     = 1;
        textureInfo.usageFlags.SetFlag(rhi::TextureUsageFlagBits::eColorAttachment);
        textureInfo.usageFlags.SetFlag(rhi::TextureUsageFlagBits::eSampled);
        m_offscreenTextures.color = m_renderDevice->CreateTexture(textureInfo, "offscreen_color");
    }
    // offscreen depth stencil
    {
        rhi::TextureInfo textureInfo{};
        textureInfo.format      = rhi::DataFormat::eD32SFloatS8UInt;
        textureInfo.type        = rhi::TextureType::e2D;
        textureInfo.width       = OFFSCREEN_TEXTURE_DIM;
        textureInfo.height      = OFFSCREEN_TEXTURE_DIM;
        textureInfo.depth       = 1;
        textureInfo.arrayLayers = 1;
        textureInfo.mipmaps     = 1;
        textureInfo.usageFlags.SetFlag(rhi::TextureUsageFlagBits::eDepthStencilAttachment);
        m_offscreenTextures.depth = m_renderDevice->CreateTexture(textureInfo, "offscreen_depth");
    }
}

void OffscreenApp::BuildRenderPipeline()
{
    GfxPipelineStates pso{};
    pso.primitiveType      = DrawPrimitiveType::eTriangleList;
    pso.rasterizationState = {};
    pso.colorBlendState    = GfxPipelineColorBlendState::CreateDisabled();
    pso.depthStencilState =
        GfxPipelineDepthStencilState::Create(true, true, CompareOperator::eLessOrEqual);
    pso.multiSampleState = {};
    pso.dynamicStates.push_back(DynamicState::eScissor);
    pso.dynamicStates.push_back(DynamicState::eViewPort);

    // offscreen
    {
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
            binding1.handles.push_back(m_sceneUBO);
            uboBindings.emplace_back(std::move(binding1));
        }
        pso.rasterizationState.cullMode = PolygonCullMode::eFront;
        rc::RenderPipelineBuilder builder(m_renderDevice);
        m_mainRPs.offscreenShaded = builder.SetVertexShader("Offscreen/phong.vert.spv")
                                        .SetFragmentShader("Offscreen/phong.frag.spv")
                                        .SetNumSamples(SampleCount::e1)
                                        .AddColorRenderTarget(rhi::DataFormat::eR8G8B8A8SRGB,
                                                              TextureUsage::eColorAttachment)
                                        .SetDepthStencilTarget(DataFormat::eD32SFloatS8UInt)
                                        .SetShaderResourceBinding(0, uboBindings)
                                        .SetPipelineState(pso)
                                        .Build();
    }

    // mirror
    {
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
            binding1.handles.push_back(m_sceneUBO);
            uboBindings.emplace_back(std::move(binding1));
        }
        std::vector<ShaderResourceBinding> textureBindings;
        {
            ShaderResourceBinding binding{};
            binding.binding = 0;
            binding.type    = ShaderResourceType::eSamplerWithTexture;
            binding.handles.push_back(m_sampler);
            binding.handles.push_back(m_offscreenTextures.color);
            textureBindings.emplace_back(std::move(binding));
        }
        pso.rasterizationState.cullMode = PolygonCullMode::eDisabled;
        rc::RenderPipelineBuilder builder(m_renderDevice);
        m_mainRPs.mirror = builder.SetVertexShader("Offscreen/mirror.vert.spv")
                               .SetFragmentShader("Offscreen/mirror.frag.spv")
                               .SetNumSamples(SampleCount::e1)
                               .AddColorRenderTarget(m_viewport->GetSwapchainFormat(),
                                                     TextureUsage::eColorAttachment)
                               .SetDepthStencilTarget(m_viewport->GetDefaultDepthStencilFormat())
                               .SetShaderResourceBinding(0, uboBindings)
                               .SetShaderResourceBinding(1, textureBindings)
                               .SetPipelineState(pso)
                               .Build();
    }
}

void OffscreenApp::LoadResources()
{
    Application::LoadResources();
    // load texture
    SamplerInfo samplerInfo{};
    m_sampler = m_renderDevice->CreateSampler(samplerInfo);
    m_texture = m_renderDevice->RequestTexture2D("wood.png");

    m_scene         = MakeUnique<sg::Scene>();
    auto gltfLoader = MakeUnique<gltf::GltfLoader>();
    gltfLoader->LoadFromFile(m_modelPath, m_scene.Get());

    uint32_t vbSize = gltfLoader->GetVertices().size() * sizeof(gltf::Vertex);
    m_vertexBuffer  = m_renderDevice->CreateVertexBuffer(
        vbSize, reinterpret_cast<const uint8_t*>(gltfLoader->GetVertices().data()));

    m_indexBuffer = m_renderDevice->CreateIndexBuffer(
        gltfLoader->GetIndices().size() * sizeof(uint32_t),
        reinterpret_cast<const uint8_t*>(gltfLoader->GetIndices().data()));

    m_sceneData.lightPos = {0.0f, 2.0f, 1.0f, 0.0f};
    m_sceneData.modelMat = Mat4(1.0f);
    m_sceneUBO           = m_renderDevice->CreateUniformBuffer(
        sizeof(SceneData), reinterpret_cast<const uint8_t*>(&m_sceneData));
}

void OffscreenApp::BuildRenderGraph()
{
    m_rdg = MakeUnique<rc::RenderGraph>();
    m_rdg->Begin();


    // offscreen pass
    {
        std::vector<RenderPassClearValue> clearValues(2);
        clearValues[0].color   = {0.2f, 0.2f, 0.2f, 1.0f};
        clearValues[1].depth   = 1.0f;
        clearValues[1].stencil = 1.0f;

        Rect2 area;
        area.minX = 0;
        area.minY = 0;
        area.maxX = OFFSCREEN_TEXTURE_DIM;
        area.maxY = OFFSCREEN_TEXTURE_DIM;

        Rect2<float> vp;
        vp.minX = 0.0f;
        vp.minY = 0.0f;
        vp.maxX = OFFSCREEN_TEXTURE_DIM;
        vp.maxY = OFFSCREEN_TEXTURE_DIM;

        std::vector<TextureHandle> renderTargets;
        renderTargets.emplace_back(m_offscreenTextures.color);
        renderTargets.emplace_back(m_offscreenTextures.depth);
        FramebufferInfo fbInfo{};
        fbInfo.width           = OFFSCREEN_TEXTURE_DIM;
        fbInfo.height          = OFFSCREEN_TEXTURE_DIM;
        fbInfo.numRenderTarget = 2;
        fbInfo.renderTargets   = renderTargets.data();
        FramebufferHandle framebuffer =
            m_viewport->GetCompatibleFramebuffer(m_mainRPs.offscreenShaded.renderPass, &fbInfo);
        auto* pass = m_rdg->AddGraphicsPassNode(m_mainRPs.offscreenShaded.renderPass, framebuffer,
                                                area, clearValues, true);
        m_rdg->DeclareTextureAccessForPass(
            pass, m_offscreenTextures.color, TextureUsage::eColorAttachment,
            TextureSubResourceRange::Color(), rc::RDGAccessType::eReadWrite);

        m_rdg->AddGraphicsPassSetScissorNode(pass, area);
        // phone
        m_rdg->AddGraphicsPassBindPipelineNode(pass, m_mainRPs.offscreenShaded.pipeline,
                                               PipelineType::eGraphics);
        m_rdg->AddGraphicsPassBindVertexBufferNode(pass, m_vertexBuffer, {0});
        m_rdg->AddGraphicsPassBindIndexBufferNode(pass, m_indexBuffer, DataFormat::eR32UInt);
        m_rdg->AddGraphicsPassSetViewportNode(pass, vp);
        for (auto* node : m_scene->GetRenderableNodes())
        {
            for (auto* subMesh : node->GetComponent<sg::Mesh>()->GetSubMeshes())
            {
                m_rdg->AddGraphicsPassDrawIndexedNode(pass, subMesh->GetIndexCount(), 1,
                                                      subMesh->GetFirstIndex(), 0, 0);
            }
        }
    }
    // mirror pass
    {
        std::vector<RenderPassClearValue> clearValues(2);
        clearValues[0].color   = {0.2f, 0.2f, 0.2f, 1.0f};
        clearValues[1].depth   = 1.0f;
        clearValues[1].stencil = 1.0f;

        Rect2 area;
        area.minX = 0;
        area.minY = 0;
        area.maxX = static_cast<int>(m_window->GetExtent2D().width);
        area.maxY = static_cast<int>(m_window->GetExtent2D().height);

        Rect2<float> vp;
        vp.minX = 0.0f;
        vp.minY = 0.0f;
        vp.maxX = (float)m_window->GetExtent2D().width;
        vp.maxY = (float)m_window->GetExtent2D().height;
        FramebufferHandle framebuffer =
            m_viewport->GetCompatibleFramebuffer(m_mainRPs.mirror.renderPass);

        auto* pass = m_rdg->AddGraphicsPassNode(m_mainRPs.mirror.renderPass, framebuffer, area,
                                                clearValues, true);
        m_rdg->DeclareTextureAccessForPass(pass, m_offscreenTextures.color, TextureUsage::eSampled,
                                           TextureSubResourceRange::Color(),
                                           rc::RDGAccessType::eRead);
        m_rdg->AddGraphicsPassSetScissorNode(pass, area);
        // phone
        m_rdg->AddGraphicsPassBindPipelineNode(pass, m_mainRPs.mirror.pipeline,
                                               PipelineType::eGraphics);
        m_rdg->AddGraphicsPassBindVertexBufferNode(pass, m_vertexBuffer, {0});
        m_rdg->AddGraphicsPassBindIndexBufferNode(pass, m_indexBuffer, DataFormat::eR32UInt);
        m_rdg->AddGraphicsPassSetViewportNode(pass, vp);
        for (auto* node : m_scene->GetRenderableNodes())
        {
            for (auto* subMesh : node->GetComponent<sg::Mesh>()->GetSubMeshes())
            {
                m_rdg->AddGraphicsPassDrawIndexedNode(pass, subMesh->GetIndexCount(), 1,
                                                      subMesh->GetFirstIndex(), 0, 0);
            }
        }
    }
    m_rdg->End();
}


int main(int argc, char** argv)
{
    platform::WindowConfig windowConfig{"offscreen", true, 1280, 720};

    Application* app = new OffscreenApp(windowConfig);

    app->Prepare();

    app->Run();

    app->Destroy();

    delete app;
}