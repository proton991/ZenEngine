#pragma once
#include "Graphics/RenderCore/V2/RenderDevice.h"
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

    RHIBuffer* GetVertexBuffer() const
    {
        return m_vertexBuffer;
    }

    RHIBuffer* GetIndexBuffer() const
    {
        return m_indexBuffer;
    }

    RHIBuffer* GetTriangleMapBuffer() const
    {
        return m_triangleMapBuffer;
    }

    auto GetNumIndices() const
    {
        return m_numIndices;
    }

    RHIBuffer* GetNodesDataSSBO() const
    {
        return m_nodeSSBO;
    }

    RHIBuffer* GetMaterialsDataSSBO() const
    {
        return m_materialSSBO;
    }

    const EnvTexture& GetEnvTexture() const
    {
        return m_envTexture;
    }

    const std::vector<RHITexture*>& GetSceneTextures() const
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
    RHIBuffer* m_nodeSSBO;

    std::vector<sg::MaterialData> m_materialsData;
    RHIBuffer* m_materialSSBO;

    SceneUniformData m_sceneUniformData{};

    RHIBuffer* m_vertexBuffer;
    RHIBuffer* m_indexBuffer;

    RHIBuffer* m_triangleMapBuffer;

    uint32_t m_numIndices{0};

    // std::vector<TextureHandle> m_sceneTextures;
    std::vector<RHITexture*> m_sceneTextures;
    std::string m_envTextureName;
    EnvTexture m_envTexture;
    RHITexture* m_defaultBaseColorTexture;
    // TextureHandle m_defaultBaseColorTexture;
};
} // namespace zen::rc