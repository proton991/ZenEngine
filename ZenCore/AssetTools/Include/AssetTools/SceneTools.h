#pragma once
#include "AssetLib/SceneLoader.h"
#include "AssetLib/SceneModelCatalog.h"

namespace zen::asset
{
// Source-enabled tools accept both processor-supported sources and runtime packages.
// Link ZenEngineAssetTools; ZenCore's LoadScene/SceneLoader remain package-only.
VectorView<const SceneFormatDescriptor> GetSourceSceneLoadFormats();
const SceneFormatDescriptor*            FindSourceSceneLoadFormat(const std::string& path);
std::string                             GetSourceSceneFileFilterExtensions();
HeapVector<SceneModelCatalogEntry>      DiscoverSourceSceneModels(const std::string& basePath);
SceneLoadResult LoadSourceScene(const std::string& path, ThreadPool<void, uint32_t>* processing = nullptr);
SceneLoadResult LoadSourceScene(const SceneImportRequest& request, ThreadPool<void, uint32_t>* processing = nullptr);

class SourceSceneLoader
{
public:
    explicit SourceSceneLoader(uint32_t concurrentLoads = 2, uint32_t processingThreads = 0);
    std::future<SceneLoadResult> LoadAsync(std::string path);
    std::future<SceneLoadResult> LoadAsync(SceneImportRequest request);

private:
    ThreadPool<void, uint32_t> m_processing;
    ThreadPool<void, uint32_t> m_imports;
};
} // namespace zen::asset
