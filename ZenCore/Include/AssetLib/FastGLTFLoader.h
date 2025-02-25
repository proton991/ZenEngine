#pragma once
#include <vector>
#include <fastgltf/core.hpp>
#include <fastgltf/types.hpp>
#include <fastgltf/tools.hpp>
#include "Types.h"
#include "Common/UniquePtr.h"

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

    void LoadFromFile(const std::string& path, sg::Scene* scene);

    const auto& GetVertices() const
    {
        return m_vertices;
    }
    const auto& GetIndices() const
    {
        return m_indices;
    }

private:
    void LoadGltfSamplers(sg::Scene* scene);

    void LoadGltfTextures(sg::Scene* scene);

    void LoadGltfMaterials(sg::Scene* scene);

    void LoadGltfMeshes(sg::Scene* scene);

    void LoadGltfRenderableNodes(sg::Scene* scene);

    void LoadGltfRenderableNodes(
        // current node index
        uint32_t nodeIndex,
        // parent node
        sg::Node* parent,
        // store results
        std::vector<UniquePtr<sg::Node>>& sgNodes,
        // access materials
        sg::Scene* scene);

    sg::Texture* LoadGltfTextureVisitor(uint32_t imageIndex);

    template <typename T> void LoadAccessor(const fastgltf::Accessor& accessor,
                                            const T*& bufferPtr,
                                            uint32_t* count              = nullptr,
                                            fastgltf::AccessorType* type = nullptr)
    {
        const fastgltf::BufferView& bufferView =
            m_gltfAsset.bufferViews[accessor.bufferViewIndex.value()];
        auto& buffer = m_gltfAsset.buffers[bufferView.bufferIndex];

        const fastgltf::sources::Array* vector =
            std::get_if<fastgltf::sources::Array>(&buffer.data);

        size_t dataOffset = bufferView.byteOffset + accessor.byteOffset;
        bufferPtr         = reinterpret_cast<const T*>(vector->bytes.data() + dataOffset);

        if (count)
        {
            *count = accessor.count;
        }
        if (type)
        {
            *type = accessor.type;
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
    std::vector<asset::Vertex> m_vertices;
    std::vector<uint32_t> m_indices;
};
} // namespace zen::asset