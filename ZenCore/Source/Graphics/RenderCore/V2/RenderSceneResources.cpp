#include "Graphics/RenderCore/V2/RenderScene.h"
#include "SceneGraph/Texture.h"
#include <algorithm>
#include <iterator>

namespace zen::rc
{
bool RenderScene::HasRequiredResources() const
{
    bool valid = m_pVertexBuffer != nullptr && m_pIndexBuffer != nullptr && m_pUVBuffer != nullptr && m_pNodeSSBO != nullptr
              && m_pMaterialSSBO != nullptr && (m_voxelTriangleCount == 0 || m_pVoxelTriangleBuffer != nullptr)
              && m_sceneTextures.size() == m_pScene->GetComponents<sg::Texture>().size()
              && m_sceneSamplers.size() == m_pScene->GetComponents<sg::Sampler>().size();

    for (RHITexture* texture : m_sceneTextures)
    {
        valid = valid && texture != nullptr && texture->GetDefaultView() != nullptr;
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
        RHIBuffer* buffers[] = {m_pVertexBuffer, m_pIndexBuffer,  m_pUVBuffer,
                                m_pNodeSSBO,     m_pMaterialSSBO, m_pVoxelTriangleBuffer};

        for (size_t index = 0; index < std::size(buffers); ++index)
        {
            if (std::find(buffers, buffers + index, buffers[index]) == buffers + index)
            {
                m_pRenderDevice->DestroyBuffer(buffers[index]);
            }
        }

        m_pVertexBuffer        = nullptr;

        m_pIndexBuffer         = nullptr;

        m_pUVBuffer            = nullptr;

        m_pNodeSSBO            = nullptr;

        m_pMaterialSSBO        = nullptr;

        m_pVoxelTriangleBuffer = nullptr;

        m_voxelTriangleCount   = 0;

        m_numIndices           = 0;

        for (size_t index = 0; index < m_sceneTextures.size(); ++index)
        {
            const HeapVector<RHITexture*>::iterator end = m_sceneTextures.begin() + index;

            if (std::find(m_sceneTextures.begin(), end, m_sceneTextures[index]) == end)
            {
                m_pRenderDevice->ReleaseSceneTexture(m_sceneTextures[index]);
            }
        }

        m_sceneTextures.clear();

        m_pRenderDevice->ReleaseSceneEnvironment(m_envTexture.Get());

        // Samplers and the default file texture belong to device caches.
        m_sceneSamplers.clear();

        m_pDefaultBaseColorTexture = nullptr;
    }
}
} // namespace zen::rc
