#pragma once
#include "../Application.h"
#include "SceneGraph/Scene.h"
#include <array>

namespace zen::rc
{
class RenderDevice;
}

class SpecializationConstantsApp : public Application
{
public:
    struct SceneData
    {
        Vec4 lightPos;
        Mat4 modelMat;
    };

    explicit SpecializationConstantsApp(const platform::WindowConfig& windowConfig);

    void Prepare() final;

    void Run() final;

    void Destroy() final;

    ~SpecializationConstantsApp() override {};

private:
    void BuildRenderPipeline();

    void LoadResources() final;

    void BuildRenderGraph() final;

    struct RenderPipelines
    {
        rc::RenderPipeline phong;
        rc::RenderPipeline toon;
        rc::RenderPipeline textured;
    } m_mainRPs;

    BufferHandle m_vertexBuffer;
    BufferHandle m_indexBuffer;

    SceneData m_sceneData;
    BufferHandle m_sceneUBO;

    UniquePtr<sg::Scene> m_scene;

    TextureHandle m_texture;
    SamplerHandle m_sampler;

    const std::string m_modelPath = "../../glTF-Sample-Models/2.0/Box/glTF/Box.gltf";
};