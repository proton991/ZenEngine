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
#include "AssetLib/GLTFLoader.h"
#include "Common/ThreadPool.h"

namespace zen
{
namespace sg
{
class Mesh;
class SubMesh;
} // namespace sg
class SceneGraphDemo : public Application
{
    struct CameraUniformData
    {
        Mat4 projViewMatrix{1.0f};
        Mat4 modelMatrix{1.0f}; // global model matrix
        Vec4 cameraPos{0.f};
    };

    struct NodeUniformData
    {
        Mat4 modelMatrix{1.0f};
        Mat4 normalMatrix{1.0f};
    };

    struct LightUniformData
    {
        Vec4 position;  // (x, y, z, lightType)
        Vec4 color;     // (r, g, b, intensity)
        Vec4 direction; // only for direction lights
    };

    struct PushConstantsData
    {
        uint32_t nodeIndex;
        uint32_t materialIndex;
        uint32_t numLights;
    };

    struct MaterialUniformData
    {
        int bcTexIndex{-1};
        int mrTexIndex{-1};
        int normalTexIndex{-1};
        int aoTexIndex{-1};
        int emissiveTexIndex{-1};
        int bcTexSet{-1};
        int mrTexSet{-1};
        int normalTexSet{-1};
        int aoTexSet{-1};
        int emissiveTexSet{-1};
        float metallicFactor{1.0f};
        float roughnessFactor{1.0f};
        Vec4 baseColorFactor{1.0f};
        Vec4 emissiveFactor{0.0f};
    };

public:
    SceneGraphDemo();

    void Prepare(const platform::WindowConfig& windowConfig) override;

    void SetupRenderGraph() override;

    void Update(float deltaTime) override;

    void Run() override;

    void LoadScene();

private:
    void TransformScene();

    void AddStaticLights();

    void FillNodeUniforms();

    void FillMaterialUniforms();

    void FillTextureArray();

    void FillLightUniforms();

    void RecordDrawCmdsSecondary(val::CommandBuffer* primaryCmdBuffer,
                                 const RDGPhysicalPass& physicalPass);

    val::CommandBuffer* RecordDrawCmdsSecondary(
        val::CommandBuffer* primaryCmdBuffer,
        uint32_t meshStart,
        uint32_t meshEnd,
        // all sub meshes and their nodes
        const std::vector<std::pair<sg::Node*, sg::SubMesh*>>& subMeshes,
        // related scene graph physical pass
        const RDGPhysicalPass& physicalPass,
        // thread id for command buffer
        uint32_t threadId = 0);

    void RecordDrawCmdsPrimary(val::CommandBuffer* primaryCmdBuffer,
                               const std::vector<sg::Node*>& nodes,
                               const RDGPhysicalPass& physicalPass);

    UniquePtr<RenderDevice> m_renderDevice;
    UniquePtr<RenderContext> m_renderContext;
    UniquePtr<RenderGraph> m_renderGraph;

    UniquePtr<ThreadPool<void, uint32_t>> m_threadPool;

    UniquePtr<ShaderManager> m_shaderManager;

    UniquePtr<TextureManager> m_textureManager;

    val::Image* m_simpleTexture;

    UniquePtr<val::Sampler> m_sampler;

    UniquePtr<VertexBuffer> m_vertexBuffer;

    UniquePtr<IndexBuffer> m_indexBuffer;

    UniquePtr<sg::Camera> m_camera;

    UniquePtr<UniformBuffer> m_cameraUBO;
    CameraUniformData m_cameraUniformData{};

    // node uniform buffer and data
    std::vector<NodeUniformData> m_nodesUniforms;
    UniquePtr<UniformBuffer> m_nodesUBO;

    // material uniform buffer and data
    std::vector<MaterialUniformData> m_materialUniforms;
    UniquePtr<UniformBuffer> m_materialUBO;

    std::vector<LightUniformData> m_lightUniforms;
    UniquePtr<UniformBuffer> m_lightUBO;

    HashMap<uint64_t, uint32_t> m_nodesUniformIndex;

    PushConstantsData m_pushConstantData;

    std::vector<val::Image*> m_textureArray;

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