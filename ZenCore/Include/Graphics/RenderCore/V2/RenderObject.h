#pragma once
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "SceneGraph/Scene.h"

namespace zen::asset
{
struct Vertex;
} // namespace zen::asset

namespace zen::rc
{
struct RenderObjectData
{
    sg::Scene* pScene;
    const asset::Vertex* pVertices;
    const uint32_t* pIndices;
    uint32_t numVertices;
    uint32_t numIndices;
};

class RenderObject
{
public:
    RenderObject(RenderDevice* pRenderDevice, const std::string& modelPath);

    const auto& GetRenderableNodes() const
    {
        return m_scene->GetRenderableNodes();
    }

    RHIBuffer* GetVertexBuffer() const
    {
        return m_pVertexBuffer;
    }

    RHIBuffer* GetIndexBuffer() const
    {
        return m_pIndexBuffer;
    }

    const auto& GetAABB() const
    {
        return m_scene->GetAABB();
    }

private:
    RenderDevice* m_pRenderDevice{nullptr};
    UniquePtr<sg::Scene> m_scene;

    std::vector<sg::MaterialData> m_materialsData;
    RHIBuffer* m_pMaterialSSBO;

    std::vector<sg::NodeData> m_nodesData;
    RHIBuffer* m_pNodeSSBO;

    RHIBuffer* m_pVertexBuffer;
    RHIBuffer* m_pIndexBuffer;

    std::vector<RHITexture*> m_sceneTextures;
};
} // namespace zen::rc