#pragma once
#include "RenderGraph.h"
#include "RenderDevice.h"
#include "SceneGraph/Scene.h"
#include "Graphics/RHI/RHIDefs.h"

using namespace zen::rhi;

namespace zen::sys
{
class Camera;
}

namespace zen::asset
{
struct Vertex;
using Index = uint32_t;
} // namespace zen::asset

namespace zen::rc
{
struct SceneData
{
    sg::Scene* scene;
    const asset::Vertex* vertices;
    const asset::Index* indices;
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

    void Init();

    void Destroy();

    void SetScene(const SceneData& sceneData);

    void DrawScene();

    void OnResize(uint32_t width, uint32_t height);

private:
    void LoadSceneMaterials();

    void LoadSceneTextures();

    void PrepareTextures();

    void PrepareBuffers();

    void BuildGraphicsPasses();

    void UpdateGraphicsPassResources();

    void BuildRenderGraph();

    void AddMeshDrawNodes(RDGPassNode* pass, const Rect2<int>& area, const Rect2<float>& viewport);

    void Clear();

    RenderDevice* m_renderDevice{nullptr};

    rhi::RHIViewport* m_viewport{nullptr};

    SkyboxRenderer* m_skyboxRenderer{nullptr};

    UniquePtr<RenderGraph> m_rdg;
    bool m_rebuildRDG{true};

    struct GraphicsPasses
    {
        GraphicsPass offscreen;
        GraphicsPass sceneLighting;
    } m_gfxPasses;

    bool m_sceneLoaded{false};
    sg::Scene* m_scene{nullptr};

    rhi::BufferHandle m_vertexBuffer;
    rhi::BufferHandle m_indexBuffer;

    PushConstantNode m_pushConstantsData{};

    SceneUniformData m_sceneUniformData{};
    BufferHandle m_sceneUBO;

    std::vector<sg::NodeData> m_nodesData;
    rhi::BufferHandle m_nodeSSBO;

    std::vector<sg::MaterialData> m_materialUniforms;
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

    EnvTexture m_envTexture;

    TextureHandle m_defaultBaseColorTexture;

    SamplerHandle m_colorSampler;
    SamplerHandle m_depthSampler;
};
} // namespace zen::rc