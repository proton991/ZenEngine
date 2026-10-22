#pragma once
#include "Templates/HeapVector.h"
#include "AssetLib/SceneAsset.h"
#include "Templates/VectorView.h"
#include "Utils/SharedPtr.h"
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

namespace zen
{
template <class FuncRetType, class... FuncArgs> class ThreadPool;
}

namespace zen::asset
{
enum class SceneImportStatus
{
    Success,
    InvalidRequest,
    UnsupportedFormat,
    SourceUnavailable,
    InvalidData,
    UnsupportedFeature,
    Cancelled,
    ResourceLimit,
    IOError
};

enum class SceneImportStage
{
    Queued,
    Reading,
    DecodingTextures,
    ProcessingMeshes,
    BakingAnimations,
    AssetReady,
    BuildingScene,
    Complete,
    Failed,
    Cancelled
};

enum class SceneDiagnosticSeverity
{
    Warning,
    Error
};

struct SceneImportDiagnostic
{
    SceneDiagnosticSeverity severity{SceneDiagnosticSeverity::Error};
    SceneImportStatus       status{SceneImportStatus::InvalidData};
    std::string             source;
    std::string             object;
    std::string             message;
};

// One control per request. Cancellation is cooperative between stages and jobs;
// a source parser or image codec already executing must finish before teardown.
class SceneImportControl
{
public:
    void Cancel()
    {
        m_cancelled.store(true, std::memory_order_release);
    }

    bool IsCancelled() const
    {
        return m_cancelled.load(std::memory_order_acquire);
    }

    SceneImportStage GetStage() const
    {
        return m_stage.load(std::memory_order_acquire);
    }

    void SetStage(SceneImportStage stage)
    {
        m_stage.store(stage, std::memory_order_release);
    }

private:
    std::atomic<bool>             m_cancelled{false};
    std::atomic<SceneImportStage> m_stage{SceneImportStage::Queued};
};

struct SceneImportSettings
{
    // UINT32_MAX selects the source's default scene. Explicit selections are
    // checked by the backend; formats without multiple scenes accept only zero.
    uint32_t sceneIndex{UINT32_MAX};
    // Used by source formats whose curves require conversion to engine tracks.
    AnimationBakeSettings animationBake;
    // Named UV sets in some FBX exports disagree with their material bindings.
    // Compatibility mode uses the mesh's first set with a diagnostic; strict
    // callers can reject such bindings. A mesh without UVs always fails.
    bool fallbackMissingNamedUVSets{true};
};

struct SceneImportRequest
{
    std::string                                       path;
    SceneImportSettings                               settings;
    SharedPtr<SceneImportControl, MultiThreadCounter> control;
    // LoadScene reuses a validated .zenasset cache when its inputs are unchanged,
    // and after a source import publishes one on a background writer (see
    // WaitForSceneAssetCacheWrites). ImportScene always runs the source importer.
    // An empty directory selects the engine tree's Cache/SceneAssets folder. After
    // each publication, least recently used caches beyond cacheBudgetBytes are removed.
    bool        useCache{true};
    std::string cacheDirectory;
    uint64_t    cacheBudgetBytes{64ull * 1024 * 1024 * 1024};
};

struct SceneImportContext
{
    ThreadPool<void, uint32_t>* processing{nullptr};
};

struct SceneImportReport
{
    std::string                       error;
    SceneImportStatus                 status{SceneImportStatus::InvalidData};
    HeapVector<SceneImportDiagnostic> diagnostics;
    std::string                       format;
    uint32_t                          importerVersion{0};
};

struct SceneImportResult : SceneImportReport
{
    SharedPtr<const SceneAsset, MultiThreadCounter> asset;
};

struct SceneLoadResult;

class ISceneImporter
{
public:
    virtual ~ISceneImporter() = default;

    // Returns owning, format-neutral CPU data. Runtime and GPU construction are separate.
    virtual SceneImportResult Import(const SceneImportRequest& request, const SceneImportContext& context) const = 0;
};

struct SceneFormatDescriptor
{
    std::string_view                   id;
    std::string_view                   displayName;
    VectorView<const std::string_view> extensions;
    uint32_t                           importerVersion{1};
};

// Built-in descriptors are immutable and shared by dispatch, discovery and UI.
VectorView<const SceneFormatDescriptor> GetSceneImportFormats();
const SceneFormatDescriptor* FindSceneFormat(const std::string& path, VectorView<const SceneFormatDescriptor> formats);
const SceneFormatDescriptor* FindSceneImportFormat(const std::string& path);
SceneImportResult            ImportScene(const SceneImportRequest& request, const SceneImportContext& context = {});
void                         SetSceneImportFailure(SceneImportResult& result,
                                                   SceneImportStatus  status,
                                                   const std::string& source,
                                                   std::string        message,
                                                   std::string        object = {});
void                         SetSceneImportFailure(SceneLoadResult&   result,
                                                   SceneImportStatus  status,
                                                   const std::string& source,
                                                   std::string        message,
                                                   std::string        object = {});
} // namespace zen::asset
