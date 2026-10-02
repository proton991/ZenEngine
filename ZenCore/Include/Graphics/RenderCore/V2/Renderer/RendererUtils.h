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

    sg::MeshTopology topology{sg::MeshTopology::Triangles};
};

// Commands and resource declarations must describe the same scene snapshot.
inline HeapVector<SceneMeshDraw> SnapshotSceneDraws(const RenderScene& scene,
                                                    uint32_t           classMask = GI_ALL,
                                                    bool trianglesOnly           = true,
                                                    bool opaqueCoverage          = true)
{
    HeapVector<SceneMeshDraw> draws;
    uint32_t                  firstTriangle = 0;

    for (sg::Node* node : scene.GetRenderableNodes())
    {
        for (sg::SubMesh* mesh : node->GetComponent<sg::Mesh>()->GetSubMeshes())
        {
            const sg::MaterialData& material = mesh->GetMaterial()->index < scene.GetMaterialsData().size()
                                                 ? scene.GetMaterialsData()[mesh->GetMaterial()->index]
                                                 : mesh->GetMaterial()->data;

            const bool solidCoverage         = material.surfaceProperties.y != static_cast<float>(sg::AlphaMode::Blend)
                                    && material.sheenColorTransmission.w == 0.0f;

            if ((scene.GetInstanceMask(node->GetRenderableIndex()) & classMask) != 0
                && (!trianglesOnly || (mesh->topology == sg::MeshTopology::Triangles && (!opaqueCoverage || solidCoverage))))
            {
                draws.push_back({node->GetRenderableIndex(), mesh->GetMaterial()->index, mesh->GetIndexCount(),
                                 mesh->GetFirstIndex(), firstTriangle, mesh->topology});
            }
            firstTriangle += mesh->topology == sg::MeshTopology::Triangles && solidCoverage ? mesh->GetIndexCount() / 3 : 0;
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

inline void BindSceneTextureArray(RDGPassDescBase&               desc,
                                  RHISampler*                    pSampler,
                                  const HeapVector<RHITexture*>& textures,
                                  const HeapVector<RHISampler*>& samplers = {})
{
    HeapVector<RHITextureView*> views;
    views.reserve(textures.size());

    for (RHITexture* pTexture : textures)
    {
        views.push_back(pTexture->GetDefaultView());
    }

    // Slot zero is the fallback; glTF sampler indices begin at slot one.
    desc.BindSeparateTexture("uTexture2DHeap", views);
    desc.BindSampler("uSamplerHeap", pSampler);

    for (uint32_t index = 0; index < samplers.size(); ++index)
    {
        desc.BindSampler("uSamplerHeap", samplers[index], index + 1);
    }
}

} // namespace zen::rc
