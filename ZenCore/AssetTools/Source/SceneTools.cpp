#include "AssetTools/SceneTools.h"
#include "AssetLib/SceneLoadCompletion.h"
#include "SceneAssetCache.h"
#include <algorithm>
#include <thread>

namespace zen::asset
{
namespace
{
HeapVector<SceneFormatDescriptor> SourceFormats()
{
    HeapVector<SceneFormatDescriptor> formats;
    for (const SceneFormatDescriptor& format : GetSceneImportFormats())
    {
        formats.push_back(format);
    }
    for (const SceneFormatDescriptor& format : GetSceneLoadFormats())
    {
        formats.push_back(format);
    }
    return formats;
}
} // namespace

VectorView<const SceneFormatDescriptor> GetSourceSceneLoadFormats()
{
    static const HeapVector<SceneFormatDescriptor> formats = SourceFormats();
    return VectorView<const SceneFormatDescriptor>(formats.data(), formats.size());
}

const SceneFormatDescriptor* FindSourceSceneLoadFormat(const std::string& path)
{
    return FindSceneFormat(path, GetSourceSceneLoadFormats());
}

std::string GetSourceSceneFileFilterExtensions()
{
    return GetSceneFileFilterExtensions(GetSourceSceneLoadFormats());
}

HeapVector<SceneModelCatalogEntry> DiscoverSourceSceneModels(const std::string& basePath)
{
    return DiscoverSceneModels(basePath, GetSourceSceneLoadFormats());
}

SceneLoadResult LoadSourceScene(const std::string& path, ThreadPool<void, uint32_t>* processing)
{
    return LoadSourceScene(SceneImportRequest{path}, processing);
}

SceneLoadResult LoadSourceScene(const SceneImportRequest& request, ThreadPool<void, uint32_t>* processing)
{
    SceneLoadResult result;
    if (FindSceneLoadFormat(request.path) != nullptr)
    {
        if (request.forceRecook)
        {
            SetSceneImportFailure(result, SceneImportStatus::InvalidRequest, request.path,
                                  "Recooking requires an original source file; packages load as stored");
            if (request.control)
            {
                request.control->SetStage(SceneImportStage::Failed);
            }
        }
        else
        {
            result = LoadScene(request, processing);
        }
    }
    else
    {
        bool              cacheHit = false;
        SceneImportResult imported = ImportSceneWithCache(request, SceneImportContext{processing}, cacheHit);
        result                     = CompleteSceneLoad(request, std::move(imported), cacheHit);
    }
    return result;
}

SourceSceneLoader::SourceSceneLoader(uint32_t concurrentLoads, uint32_t processingThreads) :
    m_processing(processingThreads == 0 ? std::max(1u, std::thread::hardware_concurrency()) : processingThreads),
    m_imports(std::max(1u, concurrentLoads))
{}

std::future<SceneLoadResult> SourceSceneLoader::LoadAsync(std::string path)
{
    return LoadAsync(SceneImportRequest{std::move(path)});
}

std::future<SceneLoadResult> SourceSceneLoader::LoadAsync(SceneImportRequest request)
{
    SceneLoadResult (*load)(const SceneImportRequest&, ThreadPool<void, uint32_t>*) = &LoadSourceScene;
    return m_imports.Push(load, std::move(request), &m_processing);
}
} // namespace zen::asset
