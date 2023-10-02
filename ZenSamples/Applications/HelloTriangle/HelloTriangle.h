#pragma once
#include "../Application.h"
#include "Graphics/Rendering/RenderGraph.h"
#include "Graphics/Rendering/RenderContext.h"
#include "Graphics/Rendering/ShaderManager.h"

namespace zen
{
class HelloTriangle : public Application
{
public:
    HelloTriangle();

    void Prepare(const platform::WindowConfig& windowConfig) override;

    void SetupRenderGraph();

    void Update(float deltaTime) override;

    void Run() override;

private:
    UniquePtr<RenderDevice>  m_renderDevice;
    UniquePtr<RenderContext> m_renderContext;
    UniquePtr<RenderGraph>   m_renderGraph;

    uint32_t m_windowWidth{0};
    uint32_t m_windowHeight{0};

    UniquePtr<ShaderManager> m_shaderManager;
};
} // namespace zen