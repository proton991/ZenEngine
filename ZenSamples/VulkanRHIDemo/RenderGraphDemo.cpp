#include "RenderGraphDemo.h"

#include "Graphics/RenderCore/V2/RenderDevice.h"

static constexpr int STAINGING_BUFFER_SIZE = 64 * 1024 * 1024;

std::vector<uint8_t> Application::LoadSpirvCode(const std::string& name)
{
    const auto path = std::string(SPV_SHADER_PATH) + name;
    std::ifstream file(path, std::ios::ate | std::ios::binary);

    VERIFY_EXPR_MSG_F(file.is_open(), "Failed to load shader file {}", path);
    //find what the size of the file is by looking up the location of the cursor
    //because the cursor is at the end, it gives the size directly in bytes
    size_t fileSize = (size_t)file.tellg();

    //spir-v expects the buffer to be on uint32, so make sure to reserve an int vector big enough for the entire file
    std::vector<uint8_t> buffer(fileSize / sizeof(uint8_t));

    //put file cursor at beginning
    file.seekg(0);

    //load the entire file into the buffer
    file.read((char*)buffer.data(), fileSize);

    //now that the file is loaded into the buffer, we can close it
    file.close();

    return buffer;
}

void Application::BuildPipeline()
{
    ShaderGroupInfo sgInfo;
    std::vector<uint8_t> vsCode = LoadSpirvCode("triangle.vert.spv");
    std::vector<uint8_t> fsCode = LoadSpirvCode("triangle_fixed.frag.spv");
    ShaderGroupInfo shaderGroupInfo{};
    auto shaderGroupSpirv = MakeRefCountPtr<ShaderGroupSPIRV>();
    shaderGroupSpirv->SetStageSPIRV(ShaderStage::eVertex, vsCode);
    shaderGroupSpirv->SetStageSPIRV(ShaderStage::eFragment, fsCode);
    ShaderUtil::ReflectShaderGroupInfo(shaderGroupSpirv, shaderGroupInfo);
    ShaderHandle shader = m_RHI->CreateShader(shaderGroupInfo);

    GfxPipelineStates pso{};
    pso.primitiveType      = DrawPrimitiveType::eTriangleList;
    pso.rasterizationState = {};
    pso.colorBlendState    = GfxPipelineColorBlendState::CreateDisabled();
    pso.depthStencilState  = {};
    pso.multiSampleState   = {};
    pso.dynamicStates.push_back(DynamicState::eScissor);
    pso.dynamicStates.push_back(DynamicState::eViewPort);


    RenderPassLayout renderPassLayout(1, false);
    renderPassLayout.AddColorRenderTarget(m_viewport->GetSwapchainFormat(),
                                          TextureUsage::eColorAttachment);
    renderPassLayout.SetColorTargetLoadStoreOp(RenderTargetLoadOp::eClear,
                                               RenderTargetStoreOp::eStore);
    m_renderPass = m_RHI->CreateRenderPass(renderPassLayout);

    TextureHandle renderBackBuffer = m_viewport->GetRenderBackBuffer();

    RenderTargetInfo colorRTInfo{};
    colorRTInfo.width           = m_window->GetExtent2D().width;
    colorRTInfo.height          = m_window->GetExtent2D().height;
    colorRTInfo.numRenderTarget = 1;
    colorRTInfo.renderTargets   = &renderBackBuffer;

    m_framebuffer   = m_RHI->CreateFramebuffer(m_renderPass, colorRTInfo);
    m_gfxPipeline   = m_RHI->CreateGfxPipeline(shader, pso, m_renderPass, 0);
    m_descriptorSet = m_RHI->CreateDescriptorSet(shader, 0);

    std::vector<ShaderResourceBinding> bindings;
    ShaderResourceBinding binding{};
    binding.binding = 0;
    binding.type    = ShaderResourceType::eUniformBuffer;
    binding.handles.push_back(m_cameraUBO);
    bindings.emplace_back(std::move(binding));
    m_RHI->UpdateDescriptorSet(m_descriptorSet, bindings);

    m_RHI->DestroyShader(shader);

    m_deletionQueue.Enqueue([=] {
        m_RHI->DestroyViewport(m_viewport);
        m_RHI->DestroyFramebuffer(m_framebuffer);
        m_RHI->DestroyRenderPass(m_renderPass);
        m_RHI->DestroyPipeline(m_gfxPipeline);
        m_RHI->DestroyDescriptorSet(m_descriptorSet);
    });
}

void Application::BuildResources()
{
    // offscreen RT
    // TextureInfo textureInfo{};
    // textureInfo.width  = m_window->GetExtent2D().width;
    // textureInfo.height = m_window->GetExtent2D().height;
    // textureInfo.fomrat = m_viewport->GetSwapchainFormat();
    // textureInfo.usageFlags.SetFlag(TextureUsageFlagBits::eColorAttachment);
    // textureInfo.type = TextureType::e2D;

    // m_offscreenRT = m_RHI->CreateTexture(textureInfo);
    // m_deletionQueue.Enqueue([=] { m_RHI->DestroyTexture(m_offscreenRT); });

    // buffers
    std::vector<Vertex> vertices = {
        {Vec3(0.5f, 0.5f, 1.0f), Vec3(1.0f, 0.0f, 0.0f), Vec2(0.0f, 1.0f)},
        {Vec3(0.5f, -0.5f, 1.0f), Vec3(0.0f, 1.0f, 0.0f), Vec2(1.0f, 1.0f)},
        {Vec3(-0.5f, 0.5f, 1.0f), Vec3(0.0f, 0.0f, 1.0f), Vec2(0.0f, 0.0f)}};
    std::vector<uint32_t> indices = {0, 1, 2};

    uint32_t vbSize = vertices.size() * sizeof(Vertex);
    m_vertexBuffer  = m_renderDevice->CreateVertexBuffer(
        vbSize, reinterpret_cast<const uint8_t*>(vertices.data()));

    m_indexBuffer = m_renderDevice->CreateIndexBuffer(
        indices.size() * sizeof(uint32_t), reinterpret_cast<const uint8_t*>(indices.data()));

    m_cameraUBO = m_renderDevice->CreateUniformBuffer(
        sizeof(CameraUniformData), reinterpret_cast<const uint8_t*>(&m_cameraData));

    m_deletionQueue.Enqueue([=] {
        m_RHI->DestroyBuffer(m_vertexBuffer);
        m_RHI->DestroyBuffer(m_indexBuffer);
        m_RHI->DestroyBuffer(m_cameraUBO);
    });
}

void Application::BuildRenderGraph()
{
    m_rdg.Begin();
    Rect2 area;
    area.minX = 0;
    area.minY = 0;
    area.maxX = (int)m_window->GetExtent2D().width;
    area.maxY = (int)m_window->GetExtent2D().height;
    RenderPassClearValue clearValue;
    clearValue.color = {0.2f, 0.2f, 0.2f, 1.0f};
    auto* mainPass = m_rdg.AddGraphicsPassNode(m_renderPass, m_framebuffer, area, clearValue, true);
    m_rdg.AddGraphicsPassBindPipelineNode(mainPass, m_gfxPipeline, PipelineType::eGraphics);
    m_rdg.AddGraphicsPassBindVertexBufferNode(mainPass, m_vertexBuffer, {0});
    m_rdg.AddGraphicsPassBindIndexBufferNode(mainPass, m_indexBuffer, DataFormat::eR32UInt);
    m_rdg.AddGraphicsPassSetViewportNode(mainPass, area);
    m_rdg.AddGraphicsPassSetScissorNode(mainPass, area);
    m_rdg.AddGraphicsPassDrawNode(mainPass, 3, 1);
    m_rdg.End();
}

Application::Application()
{
    m_renderDevice = new rc::RenderDevice(GraphicsAPIType::eVulkan, 3);
    m_renderDevice->Init();

    m_RHI = m_renderDevice->GetRHI();
    platform::WindowConfig windowConfig;
    m_window   = new platform::GlfwWindowImpl(windowConfig);
    m_viewport = m_RHI->CreateViewport(m_window, windowConfig.width, windowConfig.height, true);

    m_camera = sys::Camera::CreateUnique(Vec3{0.0f, 0.0f, -0.1f}, Vec3{0.0f, 0.0f, 0.0f},
                                         m_window->GetAspect());
    m_cameraData.projViewMatrix = m_camera->GetProjectionMatrix() * m_camera->GetViewMatrix();

    m_timer = MakeUnique<platform::Timer>();
}

void Application::Prepare()
{
    BuildResources();
    BuildPipeline();
    BuildRenderGraph();
}

void Application::Destroy()
{
    m_rdg.Destroy();
    m_deletionQueue.Flush();
    m_renderDevice->Destroy();
    delete m_renderDevice;
}

void Application::Run()
{
    while (!m_window->ShouldClose())
    {
        m_window->Update();
        m_camera->Update(static_cast<float>(m_timer->Tick()));
        m_cameraData.projViewMatrix = m_camera->GetProjectionMatrix() * m_camera->GetViewMatrix();
        m_renderDevice->Updatebuffer(m_cameraUBO, sizeof(CameraUniformData),
                                     reinterpret_cast<const uint8_t*>(&m_cameraData));
        m_renderDevice->ExecuteFrame(m_viewport, &m_rdg);
        m_renderDevice->NextFrame();
    }
}

int main(int argc, char** argv)
{
    Application application;
    application.Prepare();

    application.Run();

    application.Destroy();
}