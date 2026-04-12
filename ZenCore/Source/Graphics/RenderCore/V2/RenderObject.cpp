#include "Graphics/RenderCore/V2/RenderObject.h"
#include "AssetLib/FastGLTFLoader.h"

namespace zen::rc
{
RenderObject::RenderObject(RenderDevice* pRenderDevice, const std::string& modelPath) :
    m_pRenderDevice(pRenderDevice)
{
    m_scene         = MakeUnique<sg::Scene>();
    auto gltfLoader = MakeUnique<asset::FastGLTFLoader>();
    gltfLoader->LoadFromFile(modelPath, m_scene.Get());

    auto& vertices = gltfLoader->GetVertices();
    auto& indices  = gltfLoader->GetIndices();

    for (auto* pNode : m_scene->GetRenderableNodes())
    {
        m_nodesData.emplace_back(pNode->GetData());
    }

    m_pNodeSSBO = m_pRenderDevice->CreateStorageBuffer(
        sizeof(sg::NodeData) * m_nodesData.size(),
        reinterpret_cast<const uint8_t*>(m_nodesData.data()), "node_data_ssbo");

    m_pVertexBuffer = m_pRenderDevice->CreateVertexBuffer(
        vertices.size() * sizeof(asset::Vertex), reinterpret_cast<const uint8_t*>(vertices.data()));

    m_pIndexBuffer = m_pRenderDevice->CreateIndexBuffer(
        indices.size() * sizeof(uint32_t), reinterpret_cast<const uint8_t*>(indices.data()));
}
} // namespace zen::rc