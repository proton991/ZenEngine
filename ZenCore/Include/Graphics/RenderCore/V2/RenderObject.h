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

    const std::vector<sg::Node*>& GetRenderableNodes() const
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

    const sg::AABB& GetAABB() const
    {
        return m_scene->GetAABB();
    }

private:
    RenderDevice* m_pRenderDevice{nullptr};
    UniquePtr<sg::Scene> m_scene;

    HeapVector<sg::MaterialData> m_materialsData;
    RHIBuffer* m_pMaterialSSBO{nullptr};

    HeapVector<sg::NodeData> m_nodesData;
    RHIBuffer* m_pNodeSSBO{nullptr};

    RHIBuffer* m_pVertexBuffer{nullptr};
    RHIBuffer* m_pIndexBuffer{nullptr};

    HeapVector<RHITexture*> m_sceneTextures;
};
} // namespace zen::rc
