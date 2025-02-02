#pragma once
#include "../Application.h"
#include "SceneGraph/Scene.h"

namespace zen::rc
{
class RenderDevice;
}

class OffscreenApp : public Application
{
public:
    struct SceneData
    {
        Vec4 lightPos;
        Mat4 modelMat;
    };

    explicit OffscreenApp(const platform::WindowConfig& windowConfig);

    void Prepare() final;

    void Run() final;

    void Destroy() final;

    ~OffscreenApp() override {};

private:
    void PrepareOffscreenTextures();

    void BuildGraphicsPasses();

    void LoadResources() final;

    void BuildRenderGraph() final;

    struct GraphicsPasses
    {
        rc::GraphicsPass offscreenShaded;
        rc::GraphicsPass mirror;
    } m_gfxPasses;

    BufferHandle m_vertexBuffer;
    BufferHandle m_indexBuffer;

    SceneData m_sceneData;
    BufferHandle m_sceneUBO;

    UniquePtr<sg::Scene> m_scene;

    TextureHandle m_texture;

    struct
    {
        TextureHandle color;
        TextureHandle depth;
    } m_offscreenTextures;

    SamplerHandle m_sampler;

    const std::string m_modelPath = "../../glTF-Sample-Models/2.0/Box/glTF/Box.gltf";
};