#pragma once
#include "../Application.h"
#include "SceneGraph/Scene.h"
#include <array>
#define NUM_BOXES 16

namespace zen::rc
{
class RenderDevice;
}

class PushConstantsApp : public Application
{
public:
    struct ModelData
    {
        Mat4 model;
    };

    // Color and position data for each sphere is uploaded using push constants
    struct PushConstantData
    {
        Vec4 color;
        Vec4 position;
    };

    explicit PushConstantsApp(const platform::WindowConfig& windowConfig);

    void Prepare() final;

    void Run() final;

    void Destroy() final;

    ~PushConstantsApp() override {};

private:
    void BuildGraphicsPasses();

    void LoadResources() final;

    void BuildRenderGraph() final;

    rc::GraphicsPass m_gfxPass;

    BufferHandle m_vertexBuffer;
    BufferHandle m_indexBuffer;

    ModelData m_modelData;
    BufferHandle m_modelUBO;

    std::array<PushConstantData, NUM_BOXES> m_boxPCs;

    UniquePtr<sg::Scene> m_scene;

    const std::string m_modelPath = "../../glTF-Sample-Models/2.0/Box/glTF/Box.gltf";
};