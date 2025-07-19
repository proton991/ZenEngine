#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Systems/SceneEditor.h"
#include "SceneGraph/Camera.h"

namespace zen::rc
{
RenderScene::RenderScene(RenderDevice* renderDevice, const SceneData& sceneData) :
    m_renderDevice(renderDevice)
{
    m_camera = sceneData.camera;
    m_scene  = sceneData.scene;
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

    sys::SceneEditor::CenterAndNormalizeScene(m_scene);

    for (auto* node : m_scene->GetRenderableNodes())
    {
        m_nodesData.emplace_back(node->GetData());
    }

    m_vertexBuffer =
        m_renderDevice->CreateVertexBuffer(sceneData.numVertices * sizeof(asset::Vertex),
                                           reinterpret_cast<const uint8_t*>(sceneData.vertices));

    m_indexBuffer =
        m_renderDevice->CreateIndexBuffer(sceneData.numIndices * sizeof(uint32_t),
                                          reinterpret_cast<const uint8_t*>(sceneData.indices));

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
    auto sgMaterials = m_scene->GetComponents<sg::Material>();
    m_materialsData.reserve(sgMaterials.size());
    for (const auto* mat : sgMaterials)
    {
        m_materialsData.emplace_back(mat->data);
    }
}

void RenderScene::LoadSceneTextures()
{
    // default base color texture
    m_defaultBaseColorTexture = m_renderDevice->LoadTexture2D("wood.png");
    // scene textures
    m_renderDevice->LoadSceneTextures(m_scene, m_sceneTextures);
    // environment texture
    m_renderDevice->LoadTextureEnv(m_envTextureName, &m_envTexture);
}

void RenderScene::PrepareBuffers()
{
    std::vector<uint32_t> triangleSubMeshMap;
    for (auto* node : m_scene->GetRenderableNodes())
    {
        uint32_t subMeshIndex = 0;
        for (auto* subMesh : node->GetComponent<sg::Mesh>()->GetSubMeshes())
        {
            const int triangleCount = subMesh->GetIndexCount() / 3;
            for (uint32_t i = 0; i < triangleCount; i++)
            {
                triangleSubMeshMap.push_back(
                    m_materialsData[subMesh->GetMaterialIndex()].bcTexIndex);
            }
            subMeshIndex++;
        }
    }

    m_triangleMapBuffer = m_renderDevice->CreateStorageBuffer(
        sizeof(uint32_t) * triangleSubMeshMap.size(),
        reinterpret_cast<const uint8_t*>(triangleSubMeshMap.data()));

    // nodes data ssbo
    m_nodeSSBO =
        m_renderDevice->CreateStorageBuffer(sizeof(sg::NodeData) * m_nodesData.size(),
                                            reinterpret_cast<const uint8_t*>(m_nodesData.data()));

    // material data ssbo
    m_materialSSBO = m_renderDevice->CreateStorageBuffer(
        sizeof(sg::MaterialData) * m_materialsData.size(),
        reinterpret_cast<const uint8_t*>(m_materialsData.data()));
}

void RenderScene::Update()
{
    m_sceneUniformData.viewPos = Vec4(m_camera->GetPos(), 1.0f);
}

const sg::Camera* RenderScene::GetCamera() const
{
    return m_camera;
}

const uint8_t* RenderScene::GetCameraUniformData() const
{
    return m_camera->GetUniformData();
}

const uint8_t* RenderScene::GetSceneUniformData() const
{
    return reinterpret_cast<const uint8_t*>(&m_sceneUniformData);
}
} // namespace zen::rc