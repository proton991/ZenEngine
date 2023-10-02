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

    SetupRenderGraph();
}

void HelloTriangle::Update(float deltaTime)
{
    Application::Update(deltaTime);
}

void HelloTriangle::SetupRenderGraph()
{
    val::ShaderModule* vertexShader = m_shaderManager->RequestShader("triangle_fixed.vert.spv", VK_SHADER_STAGE_VERTEX_BIT, {});
    val::ShaderModule* fragShader   = m_shaderManager->RequestShader("triangle_fixed.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT, {});

    RDGPass* mainPass = m_renderGraph->AddPass("MainPass", RDG_QUEUE_GRAPHICS_BIT);
    mainPass->UseShaders({vertexShader, fragShader});
    RDGImage::Info outputImgInfo{};
    outputImgInfo.format = m_renderContext->GetSwapchainFormat();
    outputImgInfo.usage  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    mainPass->WriteToColorImage("Output", outputImgInfo);
    mainPass->SetOnExecute([&](val::CommandBuffer* commandBuffer) {
        auto       extent = m_renderContext->GetSwapchainExtent2D();
        VkViewport vp{};
        vp.width    = static_cast<float>(extent.width);
        vp.height   = static_cast<float>(extent.height);
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        // Set viewport dynamically
        vkCmdSetViewport(commandBuffer->GetHandle(), 0, 1, &vp);

        VkRect2D scissor{};
        scissor.extent.width  = extent.width;
        scissor.extent.height = extent.height;
        // Set scissor dynamically
        vkCmdSetScissor(commandBuffer->GetHandle(), 0, 1, &scissor);
        vkCmdDraw(commandBuffer->GetHandle(), 3, 1, 0, 0);
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
        auto* commandBuffer = m_renderContext->StartFrame(val::CommandPool::ResetMode::ResetPool);
        m_renderGraph->Execute(commandBuffer, m_renderContext.Get());
        m_renderContext->EndFrame();
    }
}
} // namespace zen
