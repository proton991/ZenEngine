#pragma once
#include "../Application.h"
#include "Graphics/Rendering/RenderGraph.h"
#include "Graphics/Rendering/RenderContext.h"
#include "Graphics/Rendering/ShaderManager.h"
#include "Graphics/Rendering/RenderBuffers.h"
#include "Platform/Timer.h"
#include "Systems/Camera.h"
#include "Common/Math.h"

namespace zen
{
struct CameraUniformData
{
    Mat4 projViewMatrix{1.0f};
};

class HelloTriangle : public Application
{
    struct Vertex
    {
        Vec3 position{0.0f, 0.0f, 0.0f};
        Vec3 color{0.0f, 0.0f, 0.0f};
    };

public:
    HelloTriangle();

    void Prepare(const platform::WindowConfig& windowConfig) override;

    void SetupRenderGraph();

    void Update(float deltaTime) override;

    void Run() override;

    void LoadModel();

private:
    UniquePtr<RenderDevice>  m_renderDevice;
    UniquePtr<RenderContext> m_renderContext;
    UniquePtr<RenderGraph>   m_renderGraph;

    uint32_t m_windowWidth{0};
    uint32_t m_windowHeight{0};

    UniquePtr<ShaderManager> m_shaderManager;

    UniquePtr<VertexBuffer> m_vertexBuffer;

    UniquePtr<IndexBuffer> m_indexBuffer;

    UniquePtr<sys::Camera> m_camera;

    UniquePtr<UniformBuffer> m_cameraUniformBuffer;
    CameraUniformData        m_cameraUniformData{};

    UniquePtr<platform::Timer> m_timer;
};
} // namespace zen