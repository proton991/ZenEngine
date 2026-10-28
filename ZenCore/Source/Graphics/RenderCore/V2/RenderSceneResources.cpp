#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/Renderer/RendererUtils.h"
#include "Graphics/RenderCore/V2/VoxelResourcePlanning.h"
#include "SceneGraph/Texture.h"
#include <algorithm>
#include <iterator>

namespace zen::rc
{
std::string RenderScene::GetResourceError(const RHIGPUInfo& gpu, const sg::Scene& scene, uint64_t vertices, uint64_t indices)
{
    std::string error = GetSceneBindlessCapacityError(gpu.bindlessHeapCapacities, scene.GetComponents<sg::Texture>().size(),
                                                      scene.GetComponents<sg::Sampler>().size());

    for (const sg::Texture* texture : scene.GetComponents<sg::Texture>())
    {
        if (error.empty() && texture->GetAtlasRegion() != asset::ImageRegion{}
            && (gpu.subTexelPrecisionBits == 0 || gpu.subTexelPrecisionBits > 23))
        {
            error = fmt::format("Scene texture '{}' has a spatial region that requires representable GPU subtexel precision.",
                                texture->GetName());
        }
    }

    // Storage helpers align their allocation in a 32-bit size. Validate before that
    // programmer assertion and before any multiplication/narrowing at scene upload.
    uint64_t uvStride = 2;

    for (const sg::VertexTextureCoordinates& sets : scene.GetAssetData().vertexTexCoords)
    {
        uvStride = std::max(uvStride, uint64_t(sets.size()));
    }

    const uint64_t uvCount = vertices > (UINT64_MAX - 1) / uvStride ? UINT64_MAX : 1 + vertices * uvStride;

    uint64_t triangles     = 0;

    for (const sg::Node* node : scene.GetRenderableNodes())
    {
        for (const sg::SubMesh* mesh : node->GetComponent<sg::Mesh>()->GetSubMeshes())
        {
            if (mesh->topology == sg::MeshTopology::Triangles && triangles <= UINT32_MAX / sizeof(glm::uvec4))
            {
                triangles += mesh->GetIndexCount() / 3;
            }
        }
    }

    struct BufferSize
    {
        const char* name;
        uint64_t    count;
        uint32_t    stride;
        bool        padded;
    };

    const BufferSize buffers[] = {
        {"vertices", std::max(vertices, uint64_t(1)), sizeof(asset::Vertex), false},
        {"indices", std::max(indices, uint64_t(1)), sizeof(uint32_t), false},
        {"texture coordinates", uvCount, sizeof(Vec4), true},
        {"nodes", std::max(uint64_t(scene.GetRenderableCount()), uint64_t(1)), sizeof(sg::NodeData), true},
        {"materials", std::max(uint64_t(scene.GetComponents<sg::Material>().size()), uint64_t(1)), sizeof(sg::MaterialData),
         true},
        {"voxel triangles", triangles, sizeof(glm::uvec4), true}};

    for (const BufferSize& buffer : buffers)
    {
        uint64_t bytes  = 0;

        const bool fits = buffer.padded
                            ? ValidateGIStorageBuffer(buffer.count, buffer.stride, gpu, bytes) == GIResourceStatus::eSuccess
                            : buffer.count <= gpu.maxStorageBufferRange / buffer.stride;

        if (error.empty() && !fits)
        {
            error = fmt::format("Scene {} exceed the supported GPU buffer size or allocation alignment limit.", buffer.name);
        }
    }

    return error;
}

bool RenderScene::HasRequiredResources() const
{
    bool valid = GetVertexBuffer() != nullptr && m_pIndexBuffer != nullptr && GetUVBuffer() != nullptr
              && GetNodesDataSSBO() != nullptr && GetMaterialsDataSSBO() != nullptr
              && (m_voxelTriangleCount == 0 || m_pVoxelTriangleBuffer != nullptr)
              && m_sceneTextures.size() == m_pScene->GetComponents<sg::Texture>().size()
              && m_sceneSamplers.size() == m_pScene->GetComponents<sg::Sampler>().size();

    for (RHITextureView* view : m_sceneTextures)
    {
        valid = valid && view != nullptr && view->GetTexture() != nullptr && view->GetTextureType() == RHITextureType::e2D
             && view->GetSubResourceRange().layerCount == 1;
    }

    for (RHISampler* sampler : m_sceneSamplers)
    {
        valid = valid && sampler != nullptr;
    }

    valid = valid && m_envTexture->IsComplete();

    return valid;
}

void RenderScene::Destroy()
{
    if (m_pRenderDevice != nullptr)
    {
        m_pRenderDevice->DestroyBuffer(m_pIndexBuffer);

        m_pRenderDevice->DestroyBuffer(m_pVoxelTriangleBuffer);

        m_vertexBuffers.Destroy(*m_pRenderDevice);

        m_uvBuffers.Destroy(*m_pRenderDevice);

        m_nodeBuffers.Destroy(*m_pRenderDevice);

        m_materialBuffers.Destroy(*m_pRenderDevice);

        m_pIndexBuffer         = nullptr;

        m_pVoxelTriangleBuffer = nullptr;

        m_voxelTriangleCount   = 0;

        m_instanceTriangleRanges.clear();

        m_numIndices = 0;

        HeapVector<RHITexture*> images;
        images.reserve(m_sceneTextures.size());
        for (RHITextureView* view : m_sceneTextures)
        {
            if (view != nullptr && std::find(images.begin(), images.end(), view->GetTexture()) == images.end())
            {
                images.push_back(view->GetTexture());
            }
        }
        m_sceneTextures.clear();
        for (RHITexture* image : images)
        {
            m_pRenderDevice->ReleaseSceneTexture(image);
        }

        m_pRenderDevice->ReleaseSceneEnvironment(m_envTexture.Get());

        // Samplers and the default file texture belong to device caches.
        m_sceneSamplers.clear();

        m_pDefaultBaseColorTexture = nullptr;
    }
}
} // namespace zen::rc
