#pragma once
#include <vector>
#include <fastgltf/core.hpp>
#include <fastgltf/types.hpp>
#include <fastgltf/tools.hpp>
#include "Types.h"
#include "Utils/UniquePtr.h"

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

    const auto& GetVertices() const
    {
        return m_vertices;
    }
    const auto& GetIndices() const
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

    template <typename T> void LoadAccessor(const fastgltf::Accessor& accessor,
                                            const T*& bufferPtr,
                                            uint32_t* pCount              = nullptr,
                                            fastgltf::AccessorType* pType = nullptr)
    {
        const fastgltf::BufferView& bufferView =
            m_gltfAsset.bufferViews[accessor.bufferViewIndex.value()];
        auto& buffer = m_gltfAsset.buffers[bufferView.bufferIndex];

        const fastgltf::sources::Array* pVector =
            std::get_if<fastgltf::sources::Array>(&buffer.data);

        size_t dataOffset = bufferView.byteOffset + accessor.byteOffset;
        bufferPtr         = reinterpret_cast<const T*>(pVector->bytes.data() + dataOffset);

        if (pCount)
        {
            *pCount = accessor.count;
        }
        if (pType)
        {
            *pType = accessor.type;
        }
    }
    std::string m_name;
    fastgltf::Options m_loadOptions;
    fastgltf::Parser m_gltfParser;
    fastgltf::Asset m_gltfAsset;
    // counters
    size_t m_vertexPos{0};
    size_t m_indexPos{0};
    // vertices and indices
    std::vector<Vertex> m_vertices;
    std::vector<uint32_t> m_indices;
};
} // namespace zen::asset