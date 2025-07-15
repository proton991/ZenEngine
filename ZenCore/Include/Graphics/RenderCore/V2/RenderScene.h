#pragma once
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Common/Math.h"
#include "SceneGraph/Scene.h"

namespace zen::sg
{
class Scene;
class Camera;
} // namespace zen::sg

namespace zen::asset
{
struct Vertex;
} // namespace zen::asset

namespace zen::rc
{
struct SceneData
{
    sg::Scene* scene;
    const asset::Vertex* vertices;
    const uint32_t* indices;
    uint32_t numVertices;
    uint32_t numIndices;
    sg::Camera* camera;
    Vec4 lightPositions[4];
    Vec4 lightColors[4];
    Vec4 lightIntensities[4];
    std::string envTextureName;
    // other scene data
};

struct SceneUniformData
{
    Vec4 lightPositions[4];
    Vec4 lightColors[4];
    Vec4 lightIntensities[4];
    Vec4 viewPos;
};

class RenderScene
{
public:
    RenderScene(RenderDevice* renderDevice, const SceneData& sceneData);

    void Init();

    void Destroy();

    void LoadSceneMaterials();

    void LoadSceneTextures();

    void PrepareBuffers();

    void Update();

    rhi::BufferHandle GetVertexBuffer() const
    {
        return m_vertexBuffer;
    }

    rhi::BufferHandle GetIndexBuffer() const
    {
        return m_indexBuffer;
    }

    rhi::BufferHandle GetTriangleMapBuffer() const
    {
        return m_triangleMapBuffer;
    }

    auto GetNumIndices() const
    {
        return m_numIndices;
    }

    rhi::BufferHandle GetNodesDataSSBO() const
    {
        return m_nodeSSBO;
    }

    rhi::BufferHandle GetMaterialsDataSSBO() const
    {
        return m_materialSSBO;
    }

    const EnvTexture& GetEnvTexture() const
    {
        return m_envTexture;
    }

    const std::vector<rhi::TextureHandle>& GetSceneTextures() const
    {
        return m_sceneTextures;
    }

    const auto& GetRenderableNodes() const
    {
        return m_scene->GetRenderableNodes();
    }

    const sg::AABB& GetAABB() const
    {
        return m_scene->GetAABB();
    }

    const sg::AABB& GetLocalAABB() const
    {
        return m_scene->GetLocalAABB();
    }

    const sg::Camera* GetCamera() const;

    const uint8_t* GetCameraUniformData() const;

    const uint8_t* GetSceneUniformData() const;

    const auto& GetMaterialsData() const
    {
        return m_materialsData;
    }

private:
    RenderDevice* m_renderDevice{nullptr};
    sg::Scene* m_scene{nullptr};
    sg::Camera* m_camera{nullptr};

    std::vector<sg::NodeData> m_nodesData;
    rhi::BufferHandle m_nodeSSBO;

    std::vector<sg::MaterialData> m_materialsData;
    rhi::BufferHandle m_materialSSBO;

    SceneUniformData m_sceneUniformData{};

    rhi::BufferHandle m_vertexBuffer;
    rhi::BufferHandle m_indexBuffer;

    rhi::BufferHandle m_triangleMapBuffer;

    uint32_t m_numIndices{0};

    std::vector<rhi::TextureHandle> m_sceneTextures;
    std::string m_envTextureName;
    EnvTexture m_envTexture;
    rhi::TextureHandle m_defaultBaseColorTexture;
};
} // namespace zen::rc