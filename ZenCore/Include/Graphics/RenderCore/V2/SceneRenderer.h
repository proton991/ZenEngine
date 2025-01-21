#pragma once
#include "RenderGraph.h"
#include "RenderDevice.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "SceneGraph/Scene.h"
#include "Graphics/RHI/RHIDefs.h"

using namespace zen::rhi;

namespace zen::sys
{
class Camera;
}

namespace zen::gltf
{
struct Vertex;
using Index = uint32_t;
} // namespace zen::gltf

namespace zen::rc
{
struct SceneData
{
    sg::Scene* scene;
    const gltf::Vertex* vertices;
    const gltf::Index* indices;
    uint32_t numVertices;
    uint32_t numIndices;
    sys::Camera* camera;
    Vec4 lightPositions[4];
    Vec4 lightColors[4];
    Vec4 lightIntensities[4];
    // other scene data
};

class SceneRenderer
{
public:
    struct NodeData
    {
        Mat4 modelMatrix{1.0f};
        Mat4 normalMatrix{1.0f};
    };

    struct MaterialData
    {
        int bcTexIndex{-1};
        int mrTexIndex{-1};
        int normalTexIndex{-1};
        int occlusionTexIndex{-1};
        int emissiveTexIndex{-1};
        int bcTexSet{-1};
        int mrTexSet{-1};
        int normalTexSet{-1};
        int aoTexSet{-1};
        int emissiveTexSet{-1};
        float metallicFactor{0.0f};
        float roughnessFactor{1.0f};
        Vec4 baseColorFactor{1.0f};
        Vec4 emissiveFactor{0.0f};
    };

    struct PushConstantsData
    {
        uint32_t nodeIndex;
        uint32_t materialIndex;
    };

    struct SceneUniformData
    {
        Vec4 lightPositions[4];
        Vec4 lightColors[4];
        Vec4 lightIntensities[4];
        Vec4 viewPos;
    };

    enum class RenderFlags
    {
        eBindTextures = 1 << 0
    };

    SceneRenderer(RenderDevice* renderDevice, RHIViewport* viewport);

    void Bake();

    void Destroy();

    void SetScene(const SceneData& sceneData);

    void DrawScene();

    void OnViewportResized()
    {
        m_rebuildRDG = true;
    }

private:
    void LoadSceneMaterials();

    void LoadSceneTextures();

    void PrepareTextures();

    void PrepareBuffers();

    void BuildRenderPipelines();

    void BuildRenderGraph();

    void AddMeshDrawNodes(RDGPassNode* pass, const Rect2<int>& area, const Rect2<float>& viewport);

    void Clear();

    void TransformScene();

    RenderDevice* m_renderDevice{nullptr};

    rhi::RHIViewport* m_viewport{nullptr};

    UniquePtr<RenderGraph> m_rdg;
    bool m_rebuildRDG{true};

    struct RenderPipelines
    {
        RenderPipeline offscreen;
        RenderPipeline sceneLighting;
    } m_mainRPs;

    bool m_sceneLoaded{false};
    sg::Scene* m_scene;

    rhi::BufferHandle m_vertexBuffer;
    rhi::BufferHandle m_indexBuffer;

    PushConstantsData m_pushConstantsData{};

    SceneUniformData m_sceneUniformData{};
    BufferHandle m_sceneUBO;


    HashMap<uint64_t, uint32_t> m_nodeUniformIndex;
    std::vector<NodeData> m_nodesData;
    rhi::BufferHandle m_nodeSSBO;

    std::vector<MaterialData> m_materialUniforms;
    rhi::BufferHandle m_materialSSBO;

    BufferHandle m_cameraUBO;
    sys::Camera* m_camera{nullptr};

    struct
    {
        TextureHandle position;
        TextureHandle normal;
        TextureHandle albedo;
        TextureHandle metallicRoughness;
        TextureHandle emissiveOcclusion;
        TextureHandle depth;
    } m_offscreenTextures;

    std::vector<TextureHandle> m_sceneTextures;

    TextureHandle m_defaultBaseColorTexture;

    SamplerHandle m_colorSampler;
    SamplerHandle m_depthSampler;
};
} // namespace zen::rc