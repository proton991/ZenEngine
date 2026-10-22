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
#include "AssetLib/Types.h"
#include "AssetLib/SceneImporter.h"
#include "AssetLib/ImportErrors.h"
#include "AssetLib/SceneAsset.h"
#include "Utils/UniquePtr.h"
#include "Templates/HeapVector.h"
#include "Utils/ThreadPool.h"

namespace zen::asset
{
class GltfAssetReader
{
public:
    // Borrow a shared processing pool, or create a bounded private pool. The caller
    // must keep that pool running and unresized during import, and must not run
    // LoadAsset on one of its workers. One loader serves one caller at a time.
    explicit GltfAssetReader(ThreadPool<void, uint32_t>* processing = nullptr);

    UniquePtr<SceneAsset> LoadAsset(const std::string&         path,
                                    const SceneImportSettings& settings = {},
                                    SceneImportControl*        control  = nullptr);

    // Empty after a successful import.
    SceneImportStatus GetStatus() const
    {
        return m_errors.Status();
    }

    std::string GetError() const
    {
        return m_errors.Message();
    }

private:
    void ImportFile(const std::filesystem::path& filePath, bool binary, SceneAsset* pScene);

    void                                             ImportAsset(SceneAsset* pScene);
    void                                             LoadGltfSamplers(SceneAsset* pScene);
    void                                             LoadGltfTextures(SceneAsset* pScene);
    void                                             LoadGltfMaterials(SceneAsset* pScene);
    void                                             LoadGltfMeshes(SceneAsset* pScene);
    void                                             LoadGltfAssetData(SceneAsset* pScene);
    void                                             LoadGltfRenderableNodes(SceneAsset* pScene);
    void                                             LoadGltfInstances(uint32_t nodeIndex, SceneAsset& scene);
    SharedPtr<const ImagePixels, MultiThreadCounter> DecodeGltfImage(uint32_t imageIndex);

    struct PrimitiveData;

    PrimitiveData ProcessPrimitive(const fastgltf::Primitive& primitive);

    const fastgltf::Accessor* GetVertexAccessor(const fastgltf::Primitive& primitive,
                                                const char*                name,
                                                fastgltf::AccessorType     type,
                                                size_t                     vertexCount) const;

    bool LoadPrimitive(const fastgltf::Primitive& primitive,
                       HeapVector<Vertex>&        vertices,
                       HeapVector<uint32_t>&      indices,
                       HeapVector<uint32_t>&      sourceVertices,
                       bool&                      flatNormals);


    std::string       m_name;
    std::string       m_json;
    std::string       m_sourceJson;
    fastgltf::Options m_loadOptions;
    fastgltf::Parser  m_gltfParser;
    fastgltf::Asset   m_gltfAsset;

    HeapVector<uint32_t>       m_linearTextureIndices;
    HeapVector<uint32_t>       m_textureIndices;
    HeapVector<uint32_t>       m_samplerIndices;
    HeapVector<uint32_t>       m_uniqueTextureIndices;
    HeapVector<Format>         m_textureFormats;
    std::map<size_t, uint32_t> m_texCoordIndices;
    ImportErrors               m_errors;
    SceneImportSettings        m_settings;

    UniquePtr<ThreadPool<void, uint32_t>> m_ownedWorkers;
    ThreadPool<void, uint32_t>*           m_workers{nullptr};
};
} // namespace zen::asset
