#pragma once
#include "../Application.h"
#include "Graphics/Rendering/RenderGraph.h"
#include "Graphics/Rendering/RenderContext.h"
#include "Graphics/Rendering/ShaderManager.h"
#include "Graphics/Rendering/RenderBuffers.h"
#include "Platform/Timer.h"
#include "Systems/Camera.h"
#include "Common/Math.h"
#include "Graphics/Rendering/TextureManager.h"
#include "AssetLib/GLTFLoader.h"

namespace zen
{
class GLTFViewer : public Application
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
    GLTFViewer();

    void Prepare(const platform::WindowConfig& windowConfig) override;

    void SetupRenderGraph() override;

    void Update(float deltaTime) override;

    void Run() override;

    void LoadModel();

    void LoadTexture();

    void RenderGLTFModel(val::CommandBuffer* pCommandBuffer);

private:
    UniquePtr<RenderDevice>  m_renderDevice;
    UniquePtr<RenderContext> m_renderContext;
    UniquePtr<RenderGraph>   m_renderGraph;

    UniquePtr<ShaderManager> m_shaderManager;

    UniquePtr<TextureManager> m_textureManager;

    val::Image* m_simpleTexture;

    UniquePtr<gltf::ModelLoader> m_gltfModelLoader;

    gltf::Model m_gltfModel;

    UniquePtr<val::Sampler> m_sampler;

    UniquePtr<VertexBuffer> m_vertexBuffer;

    UniquePtr<IndexBuffer> m_indexBuffer;

    UniquePtr<sys::Camera> m_camera;

    UniquePtr<UniformBuffer> m_cameraUniformBuffer;
    CameraUniformData        m_cameraUniformData{};

    // node uniform buffer and data
    std::vector<NodeUniformData> m_nodesUniformData;
    UniquePtr<UniformBuffer>     m_nodesUniformBuffer;

    UniquePtr<platform::Timer> m_timer;

    const std::vector<std::string> MODEL_PATHS = {
        "../../glTF-Sample-Models/2.0/Box/glTF/Box.gltf",
        "../../glTF-Sample-Models/2.0/ToyCar/glTF/ToyCar.gltf",
        "../../glTF-Sample-Models/2.0/FlightHelmet/glTF/FlightHelmet.gltf",
    };
};
} // namespace zen