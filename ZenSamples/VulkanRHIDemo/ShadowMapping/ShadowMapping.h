#pragma once
#include "../Application.h"
#include "SceneGraph/Scene.h"

namespace zen::rc
{
class RenderDevice;
}

class ShadowMappingApp : public Application
{
public:
    struct OffscreenUniformData
    {
        Mat4 depthMVP;
    };

    struct SceneUniformData
    {
        Mat4 projection;
        Mat4 view;
        Mat4 model;
        Mat4 depthBiasMVP;
        Vec3 lightPos;
        // Used for depth map visualization
        float zNear;
        float zFar;
    };

    explicit ShadowMappingApp(const platform::WindowConfig& windowConfig);

    void Prepare() final;

    void Run() final;

    void Destroy() final;

    ~ShadowMappingApp() override {};

private:
    void PrepareOffscreen();

    void BuildGraphicsPasses();

    void LoadResources() final;

    void BuildRenderGraph() final;

    void UpdateUniformBufferData();

    struct GraphicsPasses
    {
        rc::GraphicsPass offscreen;
        rc::GraphicsPass sceneShadow;
    } m_gfxPasses;

    BufferHandle m_vertexBuffer;
    BufferHandle m_indexBuffer;

    OffscreenUniformData m_offscreenUniformData;
    BufferHandle m_offscreenUBO;

    SceneUniformData m_sceneUniformData;
    BufferHandle m_sceneUBO;

    UniquePtr<sg::Scene> m_scene;

    TextureHandle m_offscreenDepthTexture;
    SamplerHandle m_depthSampler;

    struct
    {
        // Keep depth range as small as possible
        // for better shadow map precision
        float zNear = 1.0f;
        float zFar  = 100.0f;

        // Depth bias (and slope) are used to avoid shadowing artifacts
        // Constant depth bias factor (always applied)
        float depthBiasConstant = 1.25f;
        // Slope depth bias factor, applied depending on polygon's slope
        float depthBiasSlope = 1.75f;
    } m_shadowMapConfig;

    float m_lightFOV = 45.0f;

    const std::string m_modelPath = "../../glTF-Sample-Models/2.0/Box/glTF/Box.gltf";
    const uint32_t cShadowMapSize{2048};

    const DataFormat cOffscreenDepthFormat{DataFormat::eD32SFloatS8UInt};
};