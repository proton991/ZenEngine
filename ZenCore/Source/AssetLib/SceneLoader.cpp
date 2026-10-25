#include "AssetLib/SceneLoader.h"
#include "AssetLib/SceneRuntimeBuilder.h"
#include "AssetLib/SceneAssetIO.h"
#include "AssetLib/SceneLoadCompletion.h"
#include "AssetLib/SceneRuntimeBuilderInternal.h"
#include "ZenAsset/Format/Version.h"
#include <algorithm>
#include <cmath>
#include <thread>

namespace zen::asset
{
static HeapVector<SceneFormatDescriptor> LoadFormats()
{
    HeapVector<SceneFormatDescriptor> formats;
    static constexpr std::string_view extensions[]{".zenasset"};
    formats.push_back(
        {"zenasset", "ZenEngine scenes", VectorView<const std::string_view>(extensions), zenasset::PackageSchemaVersion});
    return formats;
}

VectorView<const SceneFormatDescriptor> GetSceneLoadFormats()
{
    static const HeapVector<SceneFormatDescriptor> formats = LoadFormats();
    return VectorView<const SceneFormatDescriptor>(formats.data(), formats.size());
}

const SceneFormatDescriptor* FindSceneLoadFormat(const std::string& path)
{
    return FindSceneFormat(path, GetSceneLoadFormats());
}

std::string GetSceneFileFilterExtensions()
{
    return GetSceneFileFilterExtensions(GetSceneLoadFormats());
}

std::string GetSceneFileFilterExtensions(VectorView<const SceneFormatDescriptor> formats)
{
    std::string result;
    for (const SceneFormatDescriptor& format : formats)
    {
        for (std::string_view extension : format.extensions)
        {
            if (!result.empty())
            {
                result += ';';
            }
            result += extension.substr(1);
        }
    }
    return result;
}

SceneLoadResult LoadScene(const std::string& path, ThreadPool<void, uint32_t>* processing)
{
    return LoadScene(SceneImportRequest{path}, processing);
}

SceneLoadResult LoadScene(const SceneImportRequest& request, ThreadPool<void, uint32_t>*)
{
    const SceneFormatDescriptor* format = FindSceneLoadFormat(request.path);
    SceneImportResult            imported;
    if (request.control && request.control->IsCancelled())
    {
        SetSceneImportFailure(imported, SceneImportStatus::Cancelled, request.path, "Scene load cancelled");
    }
    else if (request.path.empty())
    {
        SetSceneImportFailure(imported, SceneImportStatus::InvalidRequest, request.path, "A scene package path is required");
    }
    else if (format != nullptr)
    {
        imported = ReadSceneAssetFile(request.path, {}, request.control.Get());
    }
    else
    {
        SetSceneImportFailure(
            imported, SceneImportStatus::UnsupportedFormat, request.path,
            "Runtime loading requires a .zenasset package; source processing is available through AssetTools");
    }
    return CompleteSceneLoad(request, std::move(imported), false);
}

SceneLoadResult CompleteSceneLoad(const SceneImportRequest& request, SceneImportResult imported, bool cacheHit)
{
    SceneLoadResult result;
    if (imported.asset)
    {
        // ImportScene and the internal reader validate before returning an asset.
        result = BuildValidatedSceneRuntime(imported.asset, request.control.Get());
        // Failed and cancelled builds return neither a scene nor its asset.
        if (result.scene)
        {
            result.asset = std::move(imported.asset);
        }
        result.format          = std::move(imported.format);
        result.importerVersion = imported.importerVersion;
        for (SceneImportDiagnostic& diagnostic : imported.diagnostics)
        {
            result.diagnostics.push_back(std::move(diagnostic));
        }
    }
    else
    {
        static_cast<SceneImportReport&>(result) = std::move(static_cast<SceneImportReport&>(imported));
    }
    if (request.control && request.control->IsCancelled() && result.status != SceneImportStatus::Cancelled)
    {
        SetSceneImportFailure(result, SceneImportStatus::Cancelled, request.path, "Scene load cancelled");
    }
    if (request.control)
    {
        request.control->SetStage(result.status == SceneImportStatus::Success     ? SceneImportStage::Complete
                                  : result.status == SceneImportStatus::Cancelled ? SceneImportStage::Cancelled
                                                                                  : SceneImportStage::Failed);
    }
    if (result.scene)
    {
        result.cacheHit    = cacheHit;
        result.scene->path = request.path;
        const float extent = result.scene->scene->GetAABB().GetMaxExtent();
        if (std::isfinite(extent) && extent > 1e-6f)
        {
            result.scene->normalizationCenter = result.scene->scene->GetAABB().GetCenter();
            result.scene->normalizationScale  = 1.0f / extent;
        }
    }
    return result;
}

SceneLoader::SceneLoader(uint32_t concurrentLoads, uint32_t processingThreads) :
    m_processing(processingThreads == 0 ? std::max(1u, std::thread::hardware_concurrency()) : processingThreads),
    m_imports(std::max(1u, concurrentLoads))
{}

std::future<SceneLoadResult> SceneLoader::LoadAsync(std::string path)
{
    return LoadAsync(SceneImportRequest{std::move(path)});
}

std::future<SceneLoadResult> SceneLoader::LoadAsync(SceneImportRequest request)
{
    SceneLoadResult (*load)(const SceneImportRequest&, ThreadPool<void, uint32_t>*) = &LoadScene;
    return m_imports.Push(load, std::move(request), &m_processing);
}
} // namespace zen::asset
