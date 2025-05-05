#pragma once
#include "../Application.h"
#include "Graphics/RenderCore/RenderGraph.h"
#include "Graphics/RenderCore/RenderContext.h"
#include "Graphics/RenderCore/ShaderManager.h"
#include "Graphics/RenderCore/RenderBuffers.h"
#include "Platform/Timer.h"
#include "SceneGraph/Camera.h"
#include "Common/Math.h"
#include "Graphics/RenderCore/TextureManager.h"

namespace zen
{
class HelloTriangle : public Application
{
    struct CameraUniformData
    {
        Mat4 projViewMatrix{1.0f};
    };

    struct Vertex
    {
        Vec3 position{0.0f, 0.0f, 0.0f};
        Vec3 color{0.0f, 0.0f, 0.0f};
        Vec2 uv{0.0f, 0.0f};
    };

public:
    HelloTriangle();

    void Prepare(const platform::WindowConfig& windowConfig) override;

    void SetupRenderGraph() override;

    void Update(float deltaTime) override;

    void Run() override;

    void LoadModel();

    void LoadTexture();

private:
    UniquePtr<RenderDevice> m_renderDevice;
    UniquePtr<RenderContext> m_renderContext;
    UniquePtr<RenderGraph> m_renderGraph;

    UniquePtr<ShaderManager> m_shaderManager;

    UniquePtr<TextureManager> m_textureManager;

    val::Image* m_simpleTexture;

    UniquePtr<val::Sampler> m_sampler;

    UniquePtr<VertexBuffer> m_vertexBuffer;

    UniquePtr<IndexBuffer> m_indexBuffer;

    UniquePtr<sg::Camera> m_camera;

    UniquePtr<UniformBuffer> m_cameraUniformBuffer;
    CameraUniformData m_cameraUniformData{};

    UniquePtr<platform::Timer> m_timer;
};
} // namespace zen