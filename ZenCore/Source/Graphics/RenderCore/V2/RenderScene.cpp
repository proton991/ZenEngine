#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Systems/SceneEditor.h"
#include "SceneGraph/Camera.h"
#include "SceneGraph/SceneAnimation.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace zen::rc
{
RenderScene::RenderScene(RenderDevice* pRenderDevice, const SceneData& sceneData) :
    m_pRenderDevice(pRenderDevice),
    m_pScene(sceneData.pScene),
    m_pCamera(sceneData.pCamera),
    m_envTextureName(sceneData.envTextureName.empty() ? "papermill.ktx" : sceneData.envTextureName)
{
    try
    {

        sys::SceneEditor::CenterAndNormalizeScene(m_pScene);

        m_sceneUnitScale = m_pScene->GetAssetData().unitScale;

        m_vertices     = HeapVector<asset::Vertex>(sceneData.pVertices, sceneData.numVertices);
        m_bindVertices = m_pScene->GetAssetData().bindVertices.empty() ?
            m_vertices :
            m_pScene->GetAssetData().bindVertices;

        if (!sg::ApplySceneDeformations(*m_pScene, MakeVecView(m_bindVertices), m_vertices))
        {
            LOG_ERROR_AND_THROW("Invalid glTF deformation payload");
        }

        for (const SceneLight& light : BuildSceneLights(*m_pScene))
        {
            m_importedLightIds.push_back(m_lights.Add(light));
        }

        m_indices           = HeapVector<uint32_t>(sceneData.pIndices, sceneData.numIndices);
        const bool animated = !m_pScene->GetAssetData().animations.empty();

        m_instanceClasses =
            HeapVector<uint32_t>(m_pScene->GetRenderableCount(), animated ? GI_DYNAMIC : GI_STATIC);
        m_instanceEnabled    = HeapVector<uint32_t>(m_pScene->GetRenderableCount(), 1);
        m_authoredVisibility = HeapVector<uint32_t>(m_pScene->GetRenderableCount(), 1);

        for (const sg::Node* node : m_pScene->GetRenderableNodes())
        {
            m_authoredVisibility[node->GetRenderableIndex()] = node->IsVisible() ? 1 : 0;
        }

        m_animation = animated ? 0 : -1;

        size_t uvStride = 2;

        for (const HeapVector<Vec2>& sets : m_pScene->GetAssetData().vertexTexCoords)
        {
            uvStride = std::max(uvStride, sets.size());
        }

        m_uvCoordinates = HeapVector<Vec4>(1 + m_vertices.size() * uvStride, Vec4(0));

        m_uvCoordinates[0].x = static_cast<float>(uvStride);

        for (size_t vertex = 0; vertex < m_vertices.size(); ++vertex)
        {
            m_uvCoordinates[1 + vertex * uvStride] = Vec4(m_vertices[vertex].uv0, 0, 0);

            m_uvCoordinates[2 + vertex * uvStride] = Vec4(m_vertices[vertex].uv1, 0, 0);

            if (vertex < m_pScene->GetAssetData().vertexTexCoords.size())
            {
                const HeapVector<Vec2>& sets = m_pScene->GetAssetData().vertexTexCoords[vertex];

                for (size_t set = 0; set < sets.size(); ++set)
                {
                    m_uvCoordinates[1 + vertex * uvStride + set] = Vec4(sets[set], 0, 0);
                }
            }
        }

        m_pUVBuffer = m_pRenderDevice->CreateStorageBuffer(
            static_cast<uint32_t>(sizeof(Vec4) * m_uvCoordinates.size()),
            reinterpret_cast<const uint8_t*>(m_uvCoordinates.data()), "scene_uv_coordinates");

        m_nodesData.reserve(m_pScene->GetRenderableCount());
        for (const sg::Node* pNode : m_pScene->GetRenderableNodes())
        {
            m_nodesData.emplace_back(pNode->GetData());
        }
        m_voxelBounds = m_pScene->GetAABB();
        sg::AABB bounds;
        m_geometryReady = ComputeGeometryBounds(bounds, m_classBounds);

        const asset::Vertex emptyVertex{};

        const uint32_t emptyIndex = 0;

        m_pVertexBuffer = m_pRenderDevice->CreateVertexBuffer(
            std::max(sceneData.numVertices, 1u) * sizeof(asset::Vertex),
            reinterpret_cast<const uint8_t*>(m_vertices.empty() ? &emptyVertex :
                                                                  m_vertices.data()));

        m_pIndexBuffer = m_pRenderDevice->CreateIndexBuffer(
            std::max(sceneData.numIndices, 1u) * sizeof(uint32_t),
            reinterpret_cast<const uint8_t*>(m_indices.empty() ? &emptyIndex : m_indices.data()));

        m_numIndices = sceneData.numIndices;
    }
    catch (...)
    {
        Destroy();

        throw;
    }
}

void RenderScene::Init()
{
    LoadSceneMaterials();

    LoadSceneTextures();

    PrepareBuffers();
}

void RenderScene::LoadSceneMaterials()
{
    const std::vector<sg::Material*> sgMaterials = m_pScene->GetComponents<sg::Material>();
    m_materialsData.clear();
    m_materialsData.reserve(sgMaterials.size());
    for (const sg::Material* pMat : sgMaterials)
    {
        m_materialsData.emplace_back(pMat->data);

        const double attenuationDistance =
            static_cast<double>(pMat->data.volumeIridescence.y) * m_sceneUnitScale;

        m_materialsData.back().volumeIridescence.y =
            attenuationDistance > std::numeric_limits<float>::max() ?
            0.0f :
            static_cast<float>(attenuationDistance);
    }
}

void RenderScene::LoadSceneTextures()
{
    for (const sg::Sampler* source : m_pScene->GetComponents<sg::Sampler>())
    {
        RHISamplerCreateInfo info = RHISamplerCreateInfo::CreateLinearRepeat();
        info.minFilter            = static_cast<RHISamplerFilter>(source->minFilter);
        info.magFilter            = static_cast<RHISamplerFilter>(source->magFilter);
        info.mipFilter            = static_cast<RHISamplerFilter>(source->mipFilter);
        info.repeatU              = static_cast<RHISamplerRepeatMode>(source->wrapS);
        info.repeatV              = static_cast<RHISamplerRepeatMode>(source->wrapT);
        info.maxLod               = source->useMipmaps ? 1e20f : 0.0f;
        m_sceneSamplers.push_back(m_pRenderDevice->CreateSampler(info));
    }

    // default base color texture
    m_pDefaultBaseColorTexture = m_pRenderDevice->LoadTexture2D("wood.png");
    // scene textures
    m_pRenderDevice->LoadSceneTextures(m_pScene, m_sceneTextures);
    // environment texture
    const sg::SceneAssetData& data = m_pScene->GetAssetData();

    if (data.imageBasedLight >= 0 &&
        static_cast<size_t>(data.imageBasedLight) < data.imageBasedLights.size())
    {
        m_pRenderDevice->LoadSceneEnvironment(m_pScene, &m_envTexture);

        UpdateAuthoredEnvironment();
    }
    else
    {
        m_pRenderDevice->LoadTextureEnv(m_envTextureName, &m_envTexture);
    }
}

void RenderScene::UpdateAuthoredEnvironment()
{
    const sg::SceneAssetData& data = m_pScene->GetAssetData();

    if (data.imageBasedLight >= 0 &&
        static_cast<size_t>(data.imageBasedLight) < data.imageBasedLights.size())
    {
        const sg::ImageBasedLightAsset& light = data.imageBasedLights[data.imageBasedLight];

        const Quat rotation = glm::conjugate(glm::normalize(light.rotation));

        const Vec4 orientation(rotation.x, rotation.y, rotation.z, rotation.w);

        const float intensity = m_environmentIntensity * light.intensity;

        if (m_sceneUniformData.environment.x != intensity ||
            m_sceneUniformData.environmentOrientation != orientation)
        {
            ++m_environmentRevision;
        }

        m_authoredEnvironmentIntensity = light.intensity;

        m_sceneUniformData.environment.x = intensity;

        m_sceneUniformData.environmentOrientation = orientation;

        m_sceneUniformData.environmentProperties.x = 1.0f;
    }
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

    m_pNodeSSBO = nullptr;

    m_pRenderDevice->DestroyBuffer(m_pMaterialSSBO);

    m_pMaterialSSBO = nullptr;

    const sg::NodeData emptyNode{};

    const sg::MaterialData emptyMaterial{};

    m_pNodeSSBO = m_pRenderDevice->CreateStorageBuffer(
        static_cast<uint32_t>(sizeof(sg::NodeData) * std::max(m_nodesData.size(), size_t(1))),
        reinterpret_cast<const uint8_t*>(m_nodesData.empty() ? &emptyNode : m_nodesData.data()),
        "node_data_ssbo");
    m_pMaterialSSBO = m_pRenderDevice->CreateStorageBuffer(
        static_cast<uint32_t>(sizeof(sg::MaterialData) *
                              std::max(m_materialsData.size(), size_t(1))),
        reinterpret_cast<const uint8_t*>(m_materialsData.empty() ? &emptyMaterial :
                                                                   m_materialsData.data()),
        "material_data_ssbo");
}

HeapVector<glm::uvec4> RenderScene::BuildTriangleRecords() const
{
    HeapVector<glm::uvec4> voxelTriangles;
    for (const sg::Node* pNode : m_pScene->GetRenderableNodes())
    {
        for (const sg::SubMesh* pMesh : pNode->GetComponent<sg::Mesh>()->GetSubMeshes())
        {
            const sg::MaterialData& material = m_materialsData[pMesh->GetMaterial()->index];

            const bool solidCoverage =
                material.surfaceProperties.y != static_cast<float>(sg::AlphaMode::Blend) &&
                material.sheenColorTransmission.w == 0.0f;

            for (uint32_t i = 0; pMesh->topology == sg::MeshTopology::Triangles && solidCoverage &&
                 i + 2 < pMesh->GetIndexCount();
                 i += 3)
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
    const bool ready = CommitGeometryUpdates();

    m_lights.WriteUniforms(m_sceneUniformData);

    const sg::CameraUniformData& camera =
        *reinterpret_cast<const sg::CameraUniformData*>(m_pCamera->GetUniformData());

    const bool orthographic = camera.proj[3][3] > 0.5f;

    m_sceneUniformData.viewPos = Vec4(m_pCamera->GetPos(), orthographic ? 0.0f : 1.0f);

    const Vec3 backward(camera.view[0][2], camera.view[1][2], camera.view[2][2]);

    m_sceneUniformData.lightInfo.y = backward.x;

    m_sceneUniformData.lightInfo.z = backward.y;

    m_sceneUniformData.lightInfo.w = backward.z;

    return ready;
}

bool RenderScene::SetAnimation(uint32_t animation, bool loop)
{
    const bool valid = animation < m_pScene->GetAssetData().animations.size();

    if (valid)
    {
        m_animation = static_cast<int32_t>(animation);

        m_animationTime = 0.0f;

        m_animationLoop = loop;
    }

    return valid;
}

bool RenderScene::AdvanceAnimation(float elapsedSeconds)
{
    bool valid = std::isfinite(elapsedSeconds) && elapsedSeconds >= 0.0f;

    if (valid && m_animation >= 0)
    {
        const std::vector<sg::SceneCamera*> cameras = m_pScene->GetComponents<sg::SceneCamera>();

        sg::SceneCamera previousCamera("animation snapshot");

        if (!cameras.empty())
        {
            const sg::SceneCamera& camera = *cameras.front();

            previousCamera.worldMatrix = camera.worldMatrix;
            previousCamera.aspect      = camera.aspect;
            previousCamera.verticalFov = camera.verticalFov;
            previousCamera.nearPlane   = camera.nearPlane;
            previousCamera.farPlane    = camera.farPlane;
            previousCamera.xmag        = camera.xmag;
            previousCamera.ymag        = camera.ymag;
            previousCamera.fixedAspect = camera.fixedAspect;
            previousCamera.infiniteFar = camera.infiniteFar;
        }

        const float nextTime = m_animationTime + elapsedSeconds;

        valid = std::isfinite(nextTime) &&
            sg::EvaluateSceneAnimation(*m_pScene, static_cast<uint32_t>(m_animation), nextTime,
                                       m_animationLoop);

        HeapVector<asset::Vertex> vertices;

        if (valid)
        {
            valid = sg::ApplySceneDeformations(*m_pScene, MakeVecView(m_bindVertices), vertices);
        }

        if (valid)
        {
            m_animationTime = nextTime;

            UpdateAuthoredEnvironment();

            for (const sg::Node* node : m_pScene->GetRenderableNodes())
            {
                const uint32_t index = node->GetRenderableIndex();

                const uint32_t visible = node->IsVisible() ? 1 : 0;

                if (m_authoredVisibility[index] != visible)
                {
                    m_authoredVisibility[index] = visible;

                    m_dirtyClasses |= m_instanceClasses[index];
                }

                if (std::memcmp(&m_nodesData[index], &node->GetData(), sizeof(sg::NodeData)) != 0)
                {
                    m_nodesData[index] = node->GetData();

                    m_dirtyClasses |= m_instanceClasses[index];
                }
            }

            if (vertices.size() == m_vertices.size() && !vertices.empty() &&
                std::memcmp(vertices.data(), m_vertices.data(),
                            vertices.size() * sizeof(asset::Vertex)) != 0)
            {
                m_vertices = std::move(vertices);

                m_verticesDirty = true;

                m_dirtyClasses |= GI_DYNAMIC;

                m_dirtySurfaceClasses |= GI_DYNAMIC;
            }

            const HeapVector<HeapVector<Vec2>>& coordinates =
                m_pScene->GetAssetData().vertexTexCoords;

            const size_t stride = static_cast<size_t>(m_uvCoordinates[0].x);

            for (size_t vertex = 0; vertex < coordinates.size(); ++vertex)
            {
                for (size_t set = 0; set < coordinates[vertex].size() && set < stride; ++set)
                {
                    const size_t offset = 1 + vertex * stride + set;

                    if (offset < m_uvCoordinates.size() &&
                        Vec2(m_uvCoordinates[offset]) != coordinates[vertex][set])
                    {
                        m_uvCoordinates[offset] = Vec4(coordinates[vertex][set], 0, 0);

                        m_verticesDirty = true;

                        m_dirtyClasses |= GI_DYNAMIC;

                        m_dirtySurfaceClasses |= GI_DYNAMIC;
                    }
                }
            }

            if (m_pScene->HasComponent(typeid(sg::Light)))
            {
                const HeapVector<SceneLight> lights = BuildSceneLights(*m_pScene);

                for (size_t index = 0; index < lights.size() && index < m_importedLightIds.size();
                     ++index)
                {
                    if (m_importedLightIds[index] != 0)
                    {
                        valid &= m_lights.Update(m_importedLightIds[index], lights[index]);
                    }
                }
            }

            const std::vector<sg::Material*> materials = m_pScene->GetComponents<sg::Material>();

            for (const sg::Material* material : materials)
            {
                valid &= UpdateMaterial(material->index, material->data);
            }

            if (!cameras.empty() && m_pCamera != nullptr)
            {
                const sg::SceneCamera& camera = *cameras.front();

                if (camera.worldMatrix != previousCamera.worldMatrix ||
                    camera.aspect != previousCamera.aspect ||
                    camera.verticalFov != previousCamera.verticalFov ||
                    camera.nearPlane != previousCamera.nearPlane ||
                    camera.farPlane != previousCamera.farPlane ||
                    camera.xmag != previousCamera.xmag || camera.ymag != previousCamera.ymag ||
                    camera.fixedAspect != previousCamera.fixedAspect ||
                    camera.infiniteFar != previousCamera.infiniteFar)
                {
                    m_pCamera->SetupFromSceneCamera(camera, m_pCamera->GetAspect());
                }
            }
        }
    }

    return valid;
}

bool RenderScene::SetMaterialVariant(int32_t variant)
{
    const bool valid = m_pScene->SetMaterialVariant(variant);

    if (valid)
    {
        m_dirtyClasses |= GI_ALL;

        m_dirtySurfaceClasses |= GI_ALL;

        m_materialsDirty = true;
    }

    return valid;
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
