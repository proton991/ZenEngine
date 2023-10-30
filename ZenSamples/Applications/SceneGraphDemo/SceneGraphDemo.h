#pragma once
#include "../Application.h"
#include "Graphics/RenderCore/RenderGraph.h"
#include "Graphics/RenderCore/RenderContext.h"
#include "Graphics/RenderCore/ShaderManager.h"
#include "Graphics/RenderCore/RenderBuffers.h"
#include "Platform/Timer.h"
#include "Systems/Camera.h"
#include "Common/Math.h"
#include "Graphics/RenderCore/TextureManager.h"
#include "AssetLib/GLTFLoader.h"

namespace zen
{
class SceneGraphDemo : public Application
{
    struct CameraUniformData
    {
        Mat4 projViewMatrix{1.0f};
    };

    struct NodeUniformData
    {
        Mat4 modelMatrix{1.0f};
    };

public:
    SceneGraphDemo();

    void Prepare(const platform::WindowConfig& windowConfig) override;

    void SetupRenderGraph() override;

    void Update(float deltaTime) override;

    void Run() override;

    void LoadTexture();

    void LoadScene();

private:
    UniquePtr<RenderDevice>  m_renderDevice;
    UniquePtr<RenderContext> m_renderContext;
    UniquePtr<RenderGraph>   m_renderGraph;

    UniquePtr<ShaderManager> m_shaderManager;

    UniquePtr<TextureManager> m_textureManager;

    val::Image* m_simpleTexture;

    UniquePtr<val::Sampler> m_sampler;

    UniquePtr<VertexBuffer> m_vertexBuffer;

    UniquePtr<IndexBuffer> m_indexBuffer;

    UniquePtr<sys::Camera> m_camera;

    UniquePtr<UniformBuffer> m_cameraUniformBuffer;
    CameraUniformData        m_cameraUniformData{};

    // node uniform buffer and data
    std::vector<NodeUniformData> m_nodesUniformData;
    UniquePtr<UniformBuffer>     m_nodesUniformBuffer;

    std::unordered_map<sg::Node*, uint32_t> m_nodesUniformIndex;

    UniquePtr<platform::Timer> m_timer;

    UniquePtr<sg::Scene> m_scene;

    const std::vector<std::string> GLTF_PATHS = {
        "../../glTF-Sample-Models/2.0/Box/glTF/Box.gltf",
        "../../glTF-Sample-Models/2.0/ToyCar/glTF/ToyCar.gltf",
        "../../glTF-Sample-Models/2.0/FlightHelmet/glTF/FlightHelmet.gltf",
        "../../glTF-Sample-Models/2.0/Sponza/glTF/Sponza.gltf",
    };
};
} // namespace zen