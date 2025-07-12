#include "Offscreen.h"
#include "AssetLib/FastGLTFLoader.h"

#define OFFSCREEN_TEXTURE_DIM 512

OffscreenApp::OffscreenApp(const platform::WindowConfig& windowConfig) :
    Application(windowConfig, sg::CameraType::eFirstPerson)
{
    m_camera->SetPosition({1.0f, 1.0f, -2.0f});
}

void OffscreenApp::Prepare()
{
    Application::Prepare();
    LoadResources();
    PrepareOffscreenTextures();
    BuildGraphicsPasses();
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

void OffscreenApp::BuildGraphicsPasses()
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
        rc::GraphicsPassBuilder builder(m_renderDevice);
        m_gfxPasses.offscreenShaded =
            builder.AddShaderStage(rhi::ShaderStage::eVertex, "Offscreen/phong.vert.spv")
                .AddShaderStage(rhi::ShaderStage::eFragment, "Offscreen/phong.frag.spv")
                .SetNumSamples(SampleCount::e1)
                .AddColorRenderTarget(rhi::DataFormat::eR8G8B8A8SRGB,
                                      TextureUsage::eColorAttachment, m_offscreenTextures.color)
                .SetDepthStencilTarget(DataFormat::eD32SFloatS8UInt, m_offscreenTextures.depth)
                .SetShaderResourceBinding(0, uboBindings)
                .SetPipelineState(pso)
                .SetFramebufferInfo(m_viewport, OFFSCREEN_TEXTURE_DIM, OFFSCREEN_TEXTURE_DIM)
                .SetTag("OffScreen")
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
        rc::GraphicsPassBuilder builder(m_renderDevice);
        m_gfxPasses.mirror =
            builder.AddShaderStage(rhi::ShaderStage::eVertex, "Offscreen/mirror.vert.spv")
                .AddShaderStage(rhi::ShaderStage::eFragment, "Offscreen/mirror.frag.spv")
                .SetNumSamples(SampleCount::e1)
                .AddColorRenderTarget(m_viewport->GetSwapchainFormat(),
                                      TextureUsage::eColorAttachment,
                                      m_viewport->GetColorBackBuffer())
                .SetDepthStencilTarget(m_viewport->GetDepthStencilFormat(),
                                       m_viewport->GetDepthStencilBackBuffer())
                .SetShaderResourceBinding(0, uboBindings)
                .SetShaderResourceBinding(1, textureBindings)
                .SetPipelineState(pso)
                .SetFramebufferInfo(m_viewport)
                .SetTag("Mirror")
                .Build();
    }
}

void OffscreenApp::LoadResources()
{
    Application::LoadResources();
    // load texture
    SamplerInfo samplerInfo{};
    m_sampler = m_renderDevice->CreateSampler(samplerInfo);
    m_texture = m_renderDevice->LoadTexture2D("wood.png");

    m_scene         = MakeUnique<sg::Scene>();
    auto gltfLoader = MakeUnique<asset::FastGLTFLoader>();
    gltfLoader->LoadFromFile(m_modelPath, m_scene.Get());

    uint32_t vbSize = gltfLoader->GetVertices().size() * sizeof(asset::Vertex);
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

        auto* pass =
            m_rdg->AddGraphicsPassNode(m_gfxPasses.offscreenShaded, area, clearValues, true);
        m_rdg->DeclareTextureAccessForPass(
            pass, m_offscreenTextures.color, TextureUsage::eColorAttachment,
            TextureSubResourceRange::Color(), rhi::AccessMode::eReadWrite);

        m_rdg->AddGraphicsPassSetScissorNode(pass, area);
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

        auto* pass = m_rdg->AddGraphicsPassNode(m_gfxPasses.mirror, area, clearValues, true);
        m_rdg->DeclareTextureAccessForPass(pass, m_offscreenTextures.color, TextureUsage::eSampled,
                                           TextureSubResourceRange::Color(),
                                           rhi::AccessMode::eRead);
        m_rdg->AddGraphicsPassSetScissorNode(pass, area);
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

ZEN_RHI_DEMO_MAIN(OffscreenApp, "offscreen")