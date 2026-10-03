#pragma once
#include <atomic>
#include <filesystem>
#include <vector>
#include <map>
#include <mutex>
#include <string>
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
class Mesh;
} // namespace zen::sg

namespace zen::asset
{
// The first content error of one import. Texture-decoding workers report through it too,
// and each import stage skips its work once an error is recorded.
class GltfImportErrors
{
public:
    // Later failures keep the first message.
    void Fail(std::string message);

    bool Failed() const;

    std::string Message() const;

    void Reset();

private:
    mutable std::mutex m_mutex;
    std::string        m_message;
    std::atomic<bool>  m_failed{false};
};

class FastGLTFLoader
{
public:
    FastGLTFLoader();

    // Invalid or unsupported content returns false and leaves *pScene and the loaded geometry
    // unchanged; GetError then describes the first problem. A null scene is a caller error and
    // stops the process.
    [[nodiscard]] bool LoadFromFile(const std::string& path, sg::Scene* pScene);

    // Empty after a successful import.
    std::string GetError() const
    {
        return m_errors.Message();
    }

    const std::vector<Vertex>& GetVertices() const
    {
        return m_vertices;
    }

    const std::vector<uint32_t>& GetIndices() const
    {
        return m_indices;
    }

private:
    void ImportFile(const std::filesystem::path& filePath, bool binary, sg::Scene* pScene);

    void ImportAsset(sg::Scene* pScene);

    void LoadGltfSamplers(sg::Scene* pScene);

    void LoadGltfTextures(sg::Scene* pScene);

    void LoadGltfMaterials(sg::Scene* pScene);

    void LoadGltfMeshes(sg::Scene* pScene);

    void LoadGltfAssetData(sg::Scene* pScene);

    void LoadGltfRenderableNodes(sg::Scene* pScene);

    void LoadGltfRenderableNodes(
        // current node index
        uint32_t nodeIndex,
        // parent node
        sg::Node* pParent,
        // store results
        zen::HeapVector<UniquePtr<sg::Node>>& sgNodes,
        // access materials
        sg::Scene* pScene);

    sg::Texture* LoadGltfTextureVisitor(uint32_t imageIndex);

    HeapVector<UniquePtr<sg::Texture>> LoadGltfTextureBatch(uint32_t begin, uint32_t end);

    const fastgltf::Accessor* GetVertexAccessor(const fastgltf::Primitive& primitive,
                                                const char*                name,
                                                fastgltf::AccessorType     type,
                                                size_t                     vertexCount) const;

    bool LoadPrimitive(const fastgltf::Primitive& primitive,
                       HeapVector<Vertex>&        vertices,
                       HeapVector<uint32_t>&      indices,
                       HeapVector<uint32_t>&      sourceVertices,
                       bool&                      flatNormals);

    void CloneDeformedMesh(uint32_t meshIndex, sg::Node& node, sg::Scene* pScene);

    std::string       m_name;
    std::string       m_json;
    std::string       m_sourceJson;
    fastgltf::Options m_loadOptions;
    fastgltf::Parser  m_gltfParser;
    fastgltf::Asset   m_gltfAsset;

    HeapVector<uint32_t>                         m_linearTextureIndices;
    HeapVector<uint32_t>                         m_textureIndices;
    HeapVector<uint32_t>                         m_samplerIndices;
    HeapVector<uint32_t>                         m_uniqueTextureIndices;
    HeapVector<Format>                           m_textureFormats;
    zen::HeapVector<sg::Mesh*>                   m_meshes;
    std::map<size_t, uint32_t>                   m_texCoordIndices;
    HeapVector<HeapVector<HeapVector<uint32_t>>> m_meshSourceVertices;
    HeapVector<HeapVector<uint32_t>>             m_meshFirstVertices;
    HeapVector<HeapVector<uint8_t>>              m_meshFlatNormals;
    // vertices and indices
    std::vector<Vertex>   m_vertices;
    std::vector<uint32_t> m_indices;

    GltfImportErrors m_errors;
};
} // namespace zen::asset
