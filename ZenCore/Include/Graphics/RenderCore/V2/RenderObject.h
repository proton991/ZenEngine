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
    sg::Scene* scene;
    const asset::Vertex* vertices;
    const uint32_t* indices;
    uint32_t numVertices;
    uint32_t numIndices;
};

class RenderObject
{
public:
    RenderObject(RenderDevice* renderDevice, const std::string& modelPath);

    const auto& GetRenderableNodes() const
    {
        return m_scene->GetRenderableNodes();
    }

    rhi::RHIBuffer* GetVertexBuffer() const
    {
        return m_vertexBuffer;
    }

    rhi::RHIBuffer* GetIndexBuffer() const
    {
        return m_indexBuffer;
    }

    const auto& GetAABB() const
    {
        return m_scene->GetAABB();
    }

private:
    RenderDevice* m_renderDevice{nullptr};
    UniquePtr<sg::Scene> m_scene;

    std::vector<sg::MaterialData> m_materialsData;
    rhi::RHIBuffer* m_materialSSBO;

    std::vector<sg::NodeData> m_nodesData;
    rhi::RHIBuffer* m_nodeSSBO;

    rhi::RHIBuffer* m_vertexBuffer;
    rhi::RHIBuffer* m_indexBuffer;

    std::vector<rhi::RHITexture*> m_sceneTextures;
};
} // namespace zen::rc