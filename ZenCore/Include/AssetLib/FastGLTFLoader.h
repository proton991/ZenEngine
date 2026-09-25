#pragma once
#include <vector>
#include <fastgltf/core.hpp>
#include <fastgltf/types.hpp>
#include <fastgltf/tools.hpp>
#include "Types.h"
#include "Utils/UniquePtr.h"
#include "Templates/HeapVector.h"

namespace zen::sg
{
class Scene;
class Node;
class Texture;
} // namespace zen::sg

namespace zen::asset
{
class FastGLTFLoader
{
public:
    FastGLTFLoader();

    void LoadFromFile(const std::string& path, sg::Scene* pScene);

    const std::vector<Vertex>& GetVertices() const
    {
        return m_vertices;
    }
    const std::vector<uint32_t>& GetIndices() const
    {
        return m_indices;
    }

private:
    void LoadGltfSamplers(sg::Scene* pScene);

    void LoadGltfTextures(sg::Scene* pScene);

    void LoadGltfMaterials(sg::Scene* pScene);

    void LoadGltfMeshes(sg::Scene* pScene);

    void LoadGltfRenderableNodes(sg::Scene* pScene);

    void LoadGltfRenderableNodes(
        // current node index
        uint32_t nodeIndex,
        // parent node
        sg::Node* pParent,
        // store results
        std::vector<UniquePtr<sg::Node>>& sgNodes,
        // access materials
        sg::Scene* pScene);

    sg::Texture* LoadGltfTextureVisitor(uint32_t imageIndex);
    HeapVector<UniquePtr<sg::Texture>> LoadGltfTextureBatch(uint32_t begin, uint32_t end);

    const fastgltf::Accessor* GetVertexAccessor(const fastgltf::Primitive& primitive,
                                                const char* name,
                                                fastgltf::AccessorType type,
                                                size_t vertexCount) const;
    bool LoadPrimitive(const fastgltf::Primitive& primitive,
                       HeapVector<Vertex>& vertices,
                       HeapVector<uint32_t>& indices) const;

    std::string m_name;
    fastgltf::Options m_loadOptions;
    fastgltf::Parser m_gltfParser;
    fastgltf::Asset m_gltfAsset;
    HeapVector<uint32_t> m_linearTextureIndices;
    // vertices and indices
    std::vector<Vertex> m_vertices;
    std::vector<uint32_t> m_indices;
};
} // namespace zen::asset