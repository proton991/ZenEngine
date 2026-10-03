#include "Graphics/RenderCore/V2/RenderObject.h"
#include "AssetLib/FastGLTFLoader.h"

namespace zen::rc
{
RenderObject::RenderObject(RenderDevice* pRenderDevice, const std::string& modelPath) : m_pRenderDevice(pRenderDevice)
{
    m_scene                                     = MakeUnique<sg::Scene>();

    UniquePtr<asset::FastGLTFLoader> gltfLoader = MakeUnique<asset::FastGLTFLoader>();

    const bool loaded                           = gltfLoader->LoadFromFile(modelPath, m_scene.Get());

    // Render objects load engine assets; the renderer cannot continue without them.
    VERIFY_EXPR_MSG_F(loaded, "Cannot load required model '{}': {}", modelPath, gltfLoader->GetError());

    const std::vector<asset::Vertex>& vertices = gltfLoader->GetVertices();

    const std::vector<uint32_t>& indices       = gltfLoader->GetIndices();

    m_nodesData.reserve(m_scene->GetRenderableCount());

    for (const sg::Node* pNode : m_scene->GetRenderableNodes())
    {
        m_nodesData.emplace_back(pNode->GetData());
    }

    m_pNodeSSBO     = m_pRenderDevice->CreateStorageBuffer(sizeof(sg::NodeData) * m_nodesData.size(),
                                                           reinterpret_cast<const uint8_t*>(m_nodesData.data()), "node_data_ssbo");

    m_pVertexBuffer = m_pRenderDevice->CreateVertexBuffer(vertices.size() * sizeof(asset::Vertex),
                                                          reinterpret_cast<const uint8_t*>(vertices.data()));

    m_pIndexBuffer =
        m_pRenderDevice->CreateIndexBuffer(indices.size() * sizeof(uint32_t), reinterpret_cast<const uint8_t*>(indices.data()));
}

RenderObject::~RenderObject()
{
    m_pRenderDevice->DestroyBuffer(m_pNodeSSBO);

    m_pRenderDevice->DestroyBuffer(m_pVertexBuffer);

    m_pRenderDevice->DestroyBuffer(m_pIndexBuffer);
}
} // namespace zen::rc
