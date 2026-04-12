#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Systems/SceneEditor.h"
#include "SceneGraph/Camera.h"

namespace zen::rc
{
RenderScene::RenderScene(RenderDevice* pRenderDevice, const SceneData& sceneData) :
    m_pRenderDevice(pRenderDevice)
{
    m_pCamera = sceneData.pCamera;
    m_pScene  = sceneData.pScene;
    if (sceneData.envTextureName.empty())
    {
        // use default env texture
        m_envTextureName = "papermill.ktx";
    }

    std::memcpy(m_sceneUniformData.lightPositions, sceneData.lightPositions,
                sizeof(sceneData.lightPositions));
    std::memcpy(m_sceneUniformData.lightColors, sceneData.lightColors,
                sizeof(sceneData.lightColors));
    std::memcpy(m_sceneUniformData.lightIntensities, sceneData.lightIntensities,
                sizeof(sceneData.lightIntensities));

    sys::SceneEditor::CenterAndNormalizeScene(m_pScene);

    for (auto* pNode : m_pScene->GetRenderableNodes())
    {
        m_nodesData.emplace_back(pNode->GetData());
    }

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
    auto sgMaterials = m_pScene->GetComponents<sg::Material>();
    m_materialsData.reserve(sgMaterials.size());
    for (const auto* pMat : sgMaterials)
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
    std::vector<uint32_t> triangleSubMeshMap;
    for (auto* pNode : m_pScene->GetRenderableNodes())
    {
        uint32_t subMeshIndex = 0;
        for (auto* pSubMesh : pNode->GetComponent<sg::Mesh>()->GetSubMeshes())
        {
            const int triangleCount = pSubMesh->GetIndexCount() / 3;
            for (uint32_t i = 0; i < triangleCount; i++)
            {
                triangleSubMeshMap.push_back(
                    m_materialsData[pSubMesh->GetMaterialIndex()].bcTexIndex);
            }
            subMeshIndex++;
        }
    }

    m_pTriangleMapBuffer = m_pRenderDevice->CreateStorageBuffer(
        sizeof(uint32_t) * triangleSubMeshMap.size(),
        reinterpret_cast<const uint8_t*>(triangleSubMeshMap.data()), "triangle_map_storage_buffer");

    // nodes data ssbo
    m_pNodeSSBO = m_pRenderDevice->CreateStorageBuffer(
        sizeof(sg::NodeData) * m_nodesData.size(),
        reinterpret_cast<const uint8_t*>(m_nodesData.data()), "node_data_ssbo");

    // material data ssbo
    m_pMaterialSSBO = m_pRenderDevice->CreateStorageBuffer(
        sizeof(sg::MaterialData) * m_materialsData.size(),
        reinterpret_cast<const uint8_t*>(m_materialsData.data()), "material_data_ssbo");
}

void RenderScene::Update()
{
    m_sceneUniformData.viewPos = Vec4(m_pCamera->GetPos(), 1.0f);
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