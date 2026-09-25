#pragma once

#include "Graphics/RenderCore/V2/RenderGraph/RDGPassCompiler.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Templates/HeapVector.h"

namespace zen::rc
{
struct SceneMeshDraw
{
    uint32_t nodeIndex;
    uint32_t materialIndex;
    uint32_t indexCount;
    uint32_t firstIndex;
    uint32_t firstTriangle{0};
    uint32_t objectClass{GI_STATIC};
};

// Commands and resource declarations must describe the same scene snapshot.
inline HeapVector<SceneMeshDraw> SnapshotSceneDraws(const RenderScene& scene,
                                                    uint32_t classMask = GI_ALL)
{
    HeapVector<SceneMeshDraw> draws;
    uint32_t firstTriangle = 0;

    for (sg::Node* node : scene.GetRenderableNodes())
    {
        for (sg::SubMesh* mesh : node->GetComponent<sg::Mesh>()->GetSubMeshes())
        {
            if ((scene.GetInstanceMask(node->GetRenderableIndex()) & classMask) != 0)
            {
                draws.push_back({node->GetRenderableIndex(), mesh->GetMaterial()->index,
                                 mesh->GetIndexCount(), mesh->GetFirstIndex(), firstTriangle,
                                 scene.GetInstanceMask(node->GetRenderableIndex())});
            }
            firstTriangle += mesh->GetIndexCount() / 3;
        }
    }

    return draws;
}

inline void ClearPassResourceBindings(RDGPassDescBase& desc)
{
    desc.UAVBufferBindings.clear();
    desc.sampledTexBindings.clear();
    desc.separateTexBindings.clear();
    desc.samplerBindings.clear();
    desc.UAVTexBindings.clear();
    desc.resourceBindings.clear();
    desc.resourceStorage.clear();
    desc.bufferStorage.clear();
    desc.textureViewStorage.clear();
    desc.indirectBuffers.clear();
    desc.logicalIndirectBuffers.clear();
}

inline void BindSceneTextureArray(RDGPassDescBase& desc,
                                  RHISampler* pSampler,
                                  const HeapVector<RHITexture*>& textures)
{
    HeapVector<RHITextureView*> views;
    views.reserve(textures.size());

    for (RHITexture* pTexture : textures)
    {
        views.push_back(pTexture->GetDefaultView());
    }

    // Material texture indices address heap slots directly; scene sampling uses sampler slot 0.
    desc.BindSeparateTexture("uTexture2DHeap", views);
    desc.BindSampler("uSamplerHeap", pSampler);
}

} // namespace zen::rc
