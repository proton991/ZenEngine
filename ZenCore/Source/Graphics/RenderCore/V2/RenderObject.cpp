#include "Graphics/RenderCore/V2/RenderObject.h"
#include "AssetLib/FastGLTFLoader.h"

namespace zen::rc
{
RenderObject::RenderObject(RenderDevice* renderDevice, const std::string& modelPath) :
    m_renderDevice(renderDevice)
{
    m_scene         = MakeUnique<sg::Scene>();
    auto gltfLoader = MakeUnique<asset::FastGLTFLoader>();
    gltfLoader->LoadFromFile(modelPath, m_scene.Get());

    auto& vertices = gltfLoader->GetVertices();
    auto& indices  = gltfLoader->GetIndices();

    for (auto* node : m_scene->GetRenderableNodes())
    {
        m_nodesData.emplace_back(node->GetData());
    }

    m_nodeSSBO =
        m_renderDevice->CreateStorageBuffer(sizeof(sg::NodeData) * m_nodesData.size(),
                                            reinterpret_cast<const uint8_t*>(m_nodesData.data()));

    m_vertexBuffer = m_renderDevice->CreateVertexBuffer(
        vertices.size() * sizeof(asset::Vertex), reinterpret_cast<const uint8_t*>(vertices.data()));

    m_indexBuffer = m_renderDevice->CreateIndexBuffer(
        indices.size() * sizeof(uint32_t), reinterpret_cast<const uint8_t*>(indices.data()));
}
} // namespace zen::rc