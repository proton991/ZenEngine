#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Systems/SceneEditor.h"
#include "SceneGraph/Camera.h"

namespace zen::rc
{
RenderScene::RenderScene(RenderDevice* pRenderDevice, const SceneData& sceneData) :
    m_pRenderDevice(pRenderDevice),
    m_pScene(sceneData.pScene),
    m_pCamera(sceneData.pCamera),
    m_envTextureName(sceneData.envTextureName.empty() ? "papermill.ktx" : sceneData.envTextureName)
{

    sys::SceneEditor::CenterAndNormalizeScene(m_pScene);
    m_vertices        = HeapVector<asset::Vertex>(sceneData.pVertices, sceneData.numVertices);
    m_indices         = HeapVector<uint32_t>(sceneData.pIndices, sceneData.numIndices);
    m_instanceClasses = HeapVector<uint32_t>(m_pScene->GetRenderableCount(), GI_STATIC);
    m_instanceEnabled = HeapVector<uint32_t>(m_pScene->GetRenderableCount(), 1);

    m_nodesData.reserve(m_pScene->GetRenderableCount());
    for (const sg::Node* pNode : m_pScene->GetRenderableNodes())
    {
        m_nodesData.emplace_back(pNode->GetData());
    }
    m_voxelBounds = m_pScene->GetAABB();
    sg::AABB bounds;
    m_geometryReady = ComputeGeometryBounds(bounds, m_classBounds);

    m_pVertexBuffer =
        m_pRenderDevice->CreateVertexBuffer(sceneData.numVertices * sizeof(asset::Vertex),
                                            reinterpret_cast<const uint8_t*>(sceneData.pVertices));

    m_pIndexBuffer =
        m_pRenderDevice->CreateIndexBuffer(sceneData.numIndices * sizeof(uint32_t),
                                           reinterpret_cast<const uint8_t*>(sceneData.pIndices));

    m_numIndices = sceneData.numIndices;
}

void RenderScene::Init()
{
    LoadSceneMaterials();

    LoadSceneTextures();

    PrepareBuffers();
}

void RenderScene::Destroy()
{
    // m_renderDevice->DestroyBuffer(m_vertexBuffer);
    // m_renderDevice->DestroyBuffer(m_indexBuffer);
    // m_renderDevice->DestroyBuffer(m_nodeSSBO);
    // m_renderDevice->DestroyBuffer(m_materialSSBO);
}

void RenderScene::LoadSceneMaterials()
{
    const std::vector<sg::Material*> sgMaterials = m_pScene->GetComponents<sg::Material>();
    m_materialsData.clear();
    m_materialsData.reserve(sgMaterials.size());
    for (const sg::Material* pMat : sgMaterials)
    {
        m_materialsData.emplace_back(pMat->data);
    }
}

void RenderScene::LoadSceneTextures()
{
    // default base color texture
    m_pDefaultBaseColorTexture = m_pRenderDevice->LoadTexture2D("wood.png");
    // scene textures
    m_pRenderDevice->LoadSceneTextures(m_pScene, m_sceneTextures);
    // environment texture
    m_pRenderDevice->LoadTextureEnv(m_envTextureName, &m_envTexture);
}

void RenderScene::PrepareBuffers()
{
    const HeapVector<glm::uvec4> voxelTriangles = BuildTriangleRecords();
    m_pRenderDevice->DestroyBuffer(m_pVoxelTriangleBuffer);
    m_pVoxelTriangleBuffer = nullptr;
    m_voxelTriangleCount   = static_cast<uint32_t>(voxelTriangles.size());
    if (!voxelTriangles.empty())
    {
        m_pVoxelTriangleBuffer = m_pRenderDevice->CreateStorageBuffer(
            static_cast<uint32_t>(sizeof(glm::uvec4) * voxelTriangles.size()),
            reinterpret_cast<const uint8_t*>(voxelTriangles.data()), "voxel_triangles");
    }
    m_pRenderDevice->DestroyBuffer(m_pNodeSSBO);
    m_pRenderDevice->DestroyBuffer(m_pMaterialSSBO);
    m_pNodeSSBO = m_pRenderDevice->CreateStorageBuffer(
        static_cast<uint32_t>(sizeof(sg::NodeData) * m_nodesData.size()),
        reinterpret_cast<const uint8_t*>(m_nodesData.data()), "node_data_ssbo");
    m_pMaterialSSBO = m_pRenderDevice->CreateStorageBuffer(
        static_cast<uint32_t>(sizeof(sg::MaterialData) * m_materialsData.size()),
        reinterpret_cast<const uint8_t*>(m_materialsData.data()), "material_data_ssbo");
}

HeapVector<glm::uvec4> RenderScene::BuildTriangleRecords() const
{
    HeapVector<glm::uvec4> voxelTriangles;
    for (const sg::Node* pNode : m_pScene->GetRenderableNodes())
    {
        for (const sg::SubMesh* pMesh : pNode->GetComponent<sg::Mesh>()->GetSubMeshes())
        {
            for (uint32_t i = 0; i + 2 < pMesh->GetIndexCount(); i += 3)
            {
                voxelTriangles.push_back(glm::uvec4(
                    pMesh->GetFirstIndex() + i, pNode->GetRenderableIndex(),
                    pMesh->GetMaterial()->index, GetInstanceMask(pNode->GetRenderableIndex())));
            }
        }
    }
    return voxelTriangles;
}

bool RenderScene::Update()
{
    const bool ready           = CommitGeometryUpdates();
    m_sceneUniformData.viewPos = Vec4(m_pCamera->GetPos(), 1.0f);
    m_lights.WriteUniforms(m_sceneUniformData);
    return ready;
}

const sg::Camera* RenderScene::GetCamera() const
{
    return m_pCamera;
}

const uint8_t* RenderScene::GetCameraUniformData() const
{
    return m_pCamera->GetUniformData();
}

const uint8_t* RenderScene::GetSceneUniformData() const
{
    return reinterpret_cast<const uint8_t*>(&m_sceneUniformData);
}
} // namespace zen::rc
