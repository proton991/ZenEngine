#pragma once
#include "AssetLib/Types.h"
#include "AssetLib/SceneImporter.h"
#include "SceneGraph/Scene.h"
#include "Utils/ThreadPool.h"
#include <future>
#include <string>

namespace zen::asset
{
struct SceneAsset;
// An unpublished CPU scene and all geometry it owns. No window or GPU resources.
// It shares decoded image storage with the canonical asset but not its geometry.
struct LoadedScene
{
    UniquePtr<sg::Scene> scene;
    HeapVector<Vertex>   vertices;
    HeapVector<uint32_t> indices;
    std::string          path;
    Vec3                 normalizationCenter{0.0f};
    float                normalizationScale{1.0f};
};

struct SceneLoadResult : SceneImportReport
{
    UniquePtr<LoadedScene> scene;
    // The canonical asset the scene was built from. Holders that keep only the
    // scene release the asset's geometry; image storage stays shared.
    SharedPtr<const SceneAsset, MultiThreadCounter> asset;
    bool                                            cacheHit{false};
};

// Discovery and application file filters use the same formats as LoadScene.
VectorView<const SceneFormatDescriptor> GetSceneLoadFormats();
const SceneFormatDescriptor*            FindSceneLoadFormat(const std::string& path);
// Semicolon-separated extensions without dots, suitable for native file pickers.
std::string GetSceneFileFilterExtensions();

// Runs on the calling thread. A supplied processing pool must remain running and
// unresized until the call finishes. Do not call from that pool's workers: this
// function waits for its processing tasks.
SceneLoadResult LoadScene(const std::string& path, ThreadPool<void, uint32_t>* processing = nullptr);
SceneLoadResult LoadScene(const SceneImportRequest& request, ThreadPool<void, uint32_t>* processing = nullptr);

// Dispatches registered scene formats. Requests own their parser, scene and diagnostics.
// Call LoadAsync and destroy the service on its owner thread. Futures can be polled
// without blocking the frame. A zero processingThreads uses every hardware thread.
// Destruction drains requests before destroying the shared processing workers;
// discarding a future does not interrupt an import. GPU publication is the caller's job.
class SceneLoader
{
public:
    explicit SceneLoader(uint32_t concurrentLoads = 2, uint32_t processingThreads = 0);

    std::future<SceneLoadResult> LoadAsync(std::string path);
    std::future<SceneLoadResult> LoadAsync(SceneImportRequest request);

private:
    ThreadPool<void, uint32_t> m_processing;
    ThreadPool<void, uint32_t> m_imports;
};
} // namespace zen::asset
