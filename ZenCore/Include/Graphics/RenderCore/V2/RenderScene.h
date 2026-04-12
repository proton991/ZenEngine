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
    sg::Scene* pScene;
    const asset::Vertex* pVertices;
    const uint32_t* pIndices;
    uint32_t numVertices;
    uint32_t numIndices;
    sg::Camera* pCamera;
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
    RenderScene(RenderDevice* pRenderDevice, const SceneData& sceneData);

    void Init();

    void Destroy();

    void LoadSceneMaterials();

    void LoadSceneTextures();

    void PrepareBuffers();

    void Update();

    RHIBuffer* GetVertexBuffer() const
    {
        return m_pVertexBuffer;
    }

    RHIBuffer* GetIndexBuffer() const
    {
        return m_pIndexBuffer;
    }

    RHIBuffer* GetTriangleMapBuffer() const
    {
        return m_pTriangleMapBuffer;
    }

    auto GetNumIndices() const
    {
        return m_numIndices;
    }

    RHIBuffer* GetNodesDataSSBO() const
    {
        return m_pNodeSSBO;
    }

    RHIBuffer* GetMaterialsDataSSBO() const
    {
        return m_pMaterialSSBO;
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
        return m_pScene->GetRenderableNodes();
    }

    const sg::AABB& GetAABB() const
    {
        return m_pScene->GetAABB();
    }

    const sg::AABB& GetLocalAABB() const
    {
        return m_pScene->GetLocalAABB();
    }

    const sg::Camera* GetCamera() const;

    const uint8_t* GetCameraUniformData() const;

    const uint8_t* GetSceneUniformData() const;

    const auto& GetMaterialsData() const
    {
        return m_materialsData;
    }

private:
    RenderDevice* m_pRenderDevice{nullptr};
    sg::Scene* m_pScene{nullptr};
    sg::Camera* m_pCamera{nullptr};

    std::vector<sg::NodeData> m_nodesData;
    RHIBuffer* m_pNodeSSBO;

    std::vector<sg::MaterialData> m_materialsData;
    RHIBuffer* m_pMaterialSSBO;

    SceneUniformData m_sceneUniformData{};

    RHIBuffer* m_pVertexBuffer;
    RHIBuffer* m_pIndexBuffer;

    RHIBuffer* m_pTriangleMapBuffer;

    uint32_t m_numIndices{0};

    // std::vector<TextureHandle> m_sceneTextures;
    std::vector<RHITexture*> m_sceneTextures;
    std::string m_envTextureName;
    EnvTexture m_envTexture;
    RHITexture* m_pDefaultBaseColorTexture;
    // TextureHandle m_defaultBaseColorTexture;
};
} // namespace zen::rc