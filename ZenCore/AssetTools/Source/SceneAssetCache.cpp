#include "SceneAssetCache.h"
#include "AssetLib/SceneAssetIOInternal.h"
#include "AssetLib/ProcessedSceneIO.h"
#include "ProcessedSourceImport.h"
#include "ZenAsset/Format/Version.h"
#include "ZenAsset/Processor/Write.h"
#include "AssetLib/SceneAssetIO.h"
#include "AssetLib/AssetHash.h"
#include "AssetLib/AssetPaths.h"
#include "AssetLib/SceneImportFingerprint.h"
#include "ZenFoundation/Utils/ConditionVariable.h"
#include "Utils/Errors.h"
#include "ZenFoundation/Utils/Mutex.h"
#include "ZenFoundation/Utils/ThreadPool.h"
#include <algorithm>
#include <chrono>
#include <filesystem>

namespace zen::asset
{
namespace
{
zenasset::CompressionSettings CacheCompression()
{
    return {zenasset::TransportCodec::Zstandard, 3, true};
}
std::string CacheKey(const SceneAssetFileMetadata& metadata)
{
    return SceneAssetCacheKeyForVersion(metadata, zenasset::PackageSchemaVersion);
}

std::filesystem::path CacheDirectory(const SceneImportRequest& request)
{
    std::filesystem::path directory =
        std::filesystem::u8path(request.cacheDirectory.empty() ? std::string(ZEN_SCENE_CACHE_PATH) : request.cacheDirectory);
    return directory;
}

bool InputCurrent(const SceneAssetFileInput& input, SceneImportControl* control)
{
    AssetFileStamp  stamp;
    AssetDigest     digest  = 0;
    const bool      present = StampAssetFile(input.path, stamp);
    std::error_code error;
    const bool      valid = input.exists ? present && stamp.size == input.size && HashAssetFile(input.path, digest, control)
                                               && digest == input.contentHash
                                         : !present && !std::filesystem::exists(std::filesystem::u8path(input.path), error);
    return valid;
}

// Content decides validity, so edits that keep a file's size and write time are
// still detected. A size change rejects without reading; the remaining files are
// hashed in parallel on the processing pool when one is available.
bool InputsCurrent(const HeapVector<SceneAssetFileInput>& inputs, SceneImportControl* control, ThreadPool<void, uint32_t>* pool)
{
    bool valid = !inputs.empty();
    for (const SceneAssetFileInput& input : inputs)
    {
        AssetFileStamp stamp;
        valid = valid && (!input.exists || (StampAssetFile(input.path, stamp) && stamp.size == input.size));
    }
    HeapVector<std::future<bool>> pending;
    for (size_t index = 0; valid && pool != nullptr && index < inputs.size(); ++index)
    {
        pending.push_back(pool->Push(&InputCurrent, std::cref(inputs[index]), control));
    }
    for (size_t index = 0; valid && pool == nullptr && index < inputs.size(); ++index)
    {
        valid = InputCurrent(inputs[index], control);
    }
    for (std::future<bool>& current : pending)
    {
        valid &= current.get();
    }
    return valid;
}

bool SameLookup(const SceneAssetFileMetadata& metadata, const SceneAssetFileMetadata& lookup)
{
    SceneAssetFileMetadata identity = metadata;
    identity.inputs                 = lookup.inputs;
    return !metadata.inputs.empty() && metadata.inputs.front().path == lookup.inputs.front().path
        && CacheKey(identity) == CacheKey(lookup);
}

struct CachePublication
{
    std::string                                       path;
    SceneAssetFileMetadata                            metadata;
    std::shared_ptr<const zenasset::Scene>            asset;
    uint64_t                                          budget{0};
    SharedPtr<SceneImportControl, MultiThreadCounter> control;
    bool                                              forceRecook{false};
    bool                                              directoryLayout{false};
};

struct PublicationControl
{
    SceneImportControl* shutdown;
    SceneImportControl* request;
};

bool PublicationCancelled(void* user)
{
    const PublicationControl& control = *static_cast<const PublicationControl*>(user);
    return control.shutdown->IsCancelled() || (control.request != nullptr && control.request->IsCancelled());
}

zenasset::Status CheckPublicationInputs(SceneAssetFileMetadata& metadata, PublicationControl& control, bool fillHashes)
{
    zenasset::Status status;
    for (SceneAssetFileInput& input : metadata.inputs)
    {
        if (status.Succeeded() && PublicationCancelled(&control))
        {
            status = {zenasset::StatusCode::Cancelled, "Scene cache publication cancelled"};
        }
        if (status.Succeeded())
        {
            bool current = false;
            if (input.exists)
            {
                AssetFileStamp before;
                AssetFileStamp after;
                AssetDigest    digest = 0;
                current               = StampAssetFile(input.path, before) && before.size == input.size
                                     && (!fillHashes || before.modified == input.modified)
                                     && HashAssetFile(input.path, digest, control.shutdown, control.request)
                                     && StampAssetFile(input.path, after) && after == before && (fillHashes || digest == input.contentHash);
                if (current && fillHashes)
                {
                    input.contentHash = digest;
                }
            }
            else
            {
                std::error_code error;
                const bool      exists = std::filesystem::exists(std::filesystem::u8path(input.path), error);
                current                = !error && !exists;
            }
            if (!current || PublicationCancelled(&control))
            {
                status =
                    PublicationCancelled(&control)
                        ? zenasset::Status{zenasset::StatusCode::Cancelled, "Scene cache publication cancelled"}
                        : zenasset::Status{zenasset::StatusCode::SourceUnavailable,
                                           "Scene input changed or cannot be checked before cache publication; recook again: "
                                               + input.path};
            }
        }
    }
    return status;
}

struct PublicationInputs
{
    SceneAssetFileMetadata* metadata;
    PublicationControl*     control;
};

zenasset::Status BeforeCachePublication(void* user)
{
    PublicationInputs& inputs = *static_cast<PublicationInputs*>(user);
    return CheckPublicationInputs(*inputs.metadata, *inputs.control, false);
}

// Canonical tool opens publish after loading. Runtime loads and explicit recooks
// await only their own publication. Shutdown cancels pending writes.
class CacheWriter
{
public:
    static CacheWriter& Get()
    {
        static CacheWriter writer;
        return writer;
    }

    ~CacheWriter()
    {
        m_shutdown.Cancel();
    }

    std::future<SceneImportReport> Publish(CachePublication publication)
    {
        {
            LockAuto lock(&m_mutex);
            ++m_pending;
        }
        return m_worker.Push(&CacheWriter::Run, this, std::move(publication));
    }

    HeapVector<SceneImportDiagnostic> Wait()
    {
        HeapVector<SceneImportDiagnostic> failures;
        LockAuto                          lock(&m_mutex);
        m_idle.Wait(&m_mutex, [this] { return m_pending == 0; });
        failures = std::move(m_failures);
        return failures;
    }

private:
    SceneImportReport Run(const CachePublication& publication)
    {
        const std::chrono::steady_clock::time_point started  = std::chrono::steady_clock::now();
        SceneAssetFileMetadata                      metadata = publication.metadata;
        PublicationControl                          control{&m_shutdown, publication.control.Get()};
        // Directory imports captured the consumed bytes. The package writer checks those
        // hashes immediately before replacing the manifest. Flat imports still need hashes.
        const zenasset::Status freshness =
            publication.directoryLayout ? zenasset::Status{} : CheckPublicationInputs(metadata, control, true);
        const bool             current   = freshness.Succeeded();
        SceneImportReport      written;
        written.status = AdaptProcessedStatus(freshness.code);
        written.error  = freshness.message;
        if (current)
        {
            metadata.cacheKey = CacheKey(metadata);
            std::error_code             error;
            const std::filesystem::path path = std::filesystem::u8path(publication.path);
            std::filesystem::create_directories(path.parent_path(), error);
            zenasset::SceneWriteOptions options;
            options.metadata              = MakeProcessedMetadata(metadata);
            options.limits.maxStringBytes = SceneAssetReadLimits{}.maxStringBytes;
            options.compression                = CacheCompression();
            options.directoryLayout            = publication.directoryLayout;
            if (publication.directoryLayout)
            {
                options.maxImageWorkers = DirectoryImageWorkers;
            }
            options.progressUser               = publication.control.Get();
            options.resourceProgress           = &TrackCacheResources;
            options.partProgress               = publication.control ? &TrackCacheTransfer : nullptr;
            PublicationInputs publicationInputs{&metadata, &control};
            if (publication.forceRecook && !publication.directoryLayout)
            {
                // Explicit recooks also validate content after serialization,
                // immediately before the writer replaces the previous package.
                options.publicationUser = &publicationInputs;
                options.beforePublish   = &BeforeCachePublication;
            }
            const zenasset::Cancellation cancellation(&PublicationCancelled, &control);
            const zenasset::Status status = zenasset::WriteScene(publication.path, *publication.asset, options, &cancellation);
            written.status                = AdaptProcessedStatus(status.code);
            written.error                 = status.message;
            if (written.status == SceneImportStatus::Success)
            {
                const std::filesystem::path root =
                    publication.directoryLayout ? path.parent_path().parent_path() : path.parent_path();
                const zenasset::Status cleanup =
                    zenasset::TrimSceneDirectoryCache(PathToUtf8(root), publication.budget, publication.path);
                if (!cleanup.Succeeded())
                {
                    LOGW("{}", cleanup.message);
                }
                LOGI("Scene cache published: {} ({:.3f} s)", publication.path,
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
            }
        }
        LockAuto lock(&m_mutex);
        if (written.status != SceneImportStatus::Success && written.status != SceneImportStatus::Cancelled)
        {
            const std::string message = "Scene cache could not be saved: " + written.error;
            written.diagnostics.push_back(
                {SceneDiagnosticSeverity::Warning, written.status, metadata.inputs.front().path, publication.path, message});
            if (current)
            {
                LOGW("{}", message);
                if (!publication.forceRecook)
                {
                    m_failures.push_back(written.diagnostics.back());
                }
            }
        }
        --m_pending;
        m_idle.NotifyAll();
        return written;
    }

    SceneImportControl                m_shutdown;
    Mutex                             m_mutex;
    ConditionVariable                 m_idle;
    uint32_t                          m_pending{0};
    HeapVector<SceneImportDiagnostic> m_failures;
    // Declared last: destroyed first, joining the worker before the state above.
    ThreadPool<void, uint32_t> m_worker{1};
};
} // namespace

SceneImportResult ImportSceneWithCache(const SceneImportRequest&                  request,
                                       const SceneImportContext&                  context,
                                       bool&                                      cacheHit,
                                       SharedPtr<SceneAsset, MultiThreadCounter>* runtimeOwner)
{
    SceneImportResult result;
    if (runtimeOwner != nullptr)
    {
        runtimeOwner->Reset();
    }
    cacheHit                            = false;
    const SceneFormatDescriptor* format = FindSceneImportFormat(request.path);
    SceneAssetFileMetadata       lookup;
    std::string                  cachePath;
    AssetFileStamp               sourceStamp;
    std::shared_ptr<const zenasset::Package> previous;
    SceneAssetReadLimits                     readLimits;
    readLimits.readWorkers                  = request.readWorkers;
    const zenasset::Status       validation = ValidateSourceProcessing(request);
    if (!validation.Succeeded())
    {
        SetSceneImportFailure(result, AdaptProcessedStatus(validation.code), request.path, validation.message);
        if (request.control)
        {
            request.control->SetStage(SceneImportStage::Failed);
        }
    }
    if (validation.Succeeded() && request.useCache && format != nullptr
        && (!request.control || !request.control->IsCancelled()))
    {
        if (request.control)
        {
            request.control->SetStage(SceneImportStage::Reading);
        }
        const std::string           source    = AbsoluteUtf8Path(std::filesystem::u8path(request.path));
        const std::filesystem::path directory = CacheDirectory(request);
        if (!source.empty() && !directory.empty() && StampAssetFile(request.path, sourceStamp))
        {
            lookup.sourceFormat    = std::string(format->id);
            lookup.importerVersion = format->importerVersion;
            lookup.buildFingerprint =
                std::string(ZEN_SCENE_IMPORT_FINGERPRINT) + ":" + std::string(zenasset::LibraryBuildFingerprint());
            lookup.processorVersion = zenasset::ProcessorVersion;
            lookup.settings         = request.settings;
            lookup.profile          = request.profile;
            lookup.processing       = request.processing;
            lookup.inputs.push_back({source});
            cachePath = PathToUtf8(directory / (CacheKey(lookup) + ".zenasset"));
            if (request.directoryCache)
            {
                const std::string      flatPath = cachePath;
                const zenasset::Status resolved = zenasset::SceneDirectoryManifest(
                    PathToUtf8(directory), source, MakeProcessedMetadata(lookup), CacheCompression(), cachePath);
                if (!resolved.Succeeded())
                {
                    cachePath.clear();
                    LOGW("Cannot claim scene cache directory: {}", resolved.message);
                }
                else
                {
                    const zenasset::Cancellation  cancellation(&ProcessedOperationCancelled, request.control.Get());
                    const zenasset::PackageResult current = zenasset::OpenPackage(cachePath, {}, &cancellation);
                    if (current.status.Succeeded() && !zenasset::HasProcessingOperations(request.processing))
                    {
                        const zenasset::SceneMetadataResult old =
                            zenasset::ReadSceneMetadata(*current.package, {}, &cancellation);
                        if (old.status.Succeeded() && old.metadata.buildFingerprint == lookup.buildFingerprint
                            && !zenasset::HasProcessingOperations(old.metadata.processing))
                        {
                            previous = current.package;
                        }
                    }
                    if (!request.forceRecook && current.status.code == zenasset::StatusCode::SourceUnavailable)
                    {
                        zenasset::PackageResult flat = zenasset::OpenPackage(flatPath, {}, &cancellation);
                        if (flat.status.Succeeded())
                        {
                            const zenasset::SceneMetadataResult old =
                                zenasset::ReadSceneMetadata(*flat.package, {}, &cancellation);
                            const SceneAssetFileMetadata metadata = AdaptProcessedMetadata(old.metadata);
                            if (old.status.Succeeded() && SameLookup(metadata, lookup)
                                && metadata.cacheKey == CacheKey(metadata)
                                && InputsCurrent(metadata.inputs, request.control.Get(), context.processing))
                            {
                                zenasset::SceneWriteOptions options;
                                options.directoryLayout = true;
                                options.metadata        = old.metadata;
                                options.compression     = old.metadata.compression;
                                options.limits          = ProcessedLimits(readLimits);
                                const zenasset::Status migrated =
                                    zenasset::MigrateSceneDirectory(cachePath, flat.package, options, &cancellation);
                                if (migrated.Succeeded())
                                {
                                    flat.package.reset();
                                    const zenasset::Status cleanup = zenasset::TrimSceneDirectoryCache(
                                        PathToUtf8(directory), request.cacheBudgetBytes, cachePath);
                                    if (!cleanup.Succeeded())
                                    {
                                        LOGW("{}", cleanup.message);
                                    }
                                }
                                else
                                {
                                    LOGW("Scene cache migration failed: {}", migrated.message);
                                }
                            }
                        }
                    }
                }
            }
            // Metadata inspection avoids allocating stale bulk data. The full
            // reader validates checksum, budgets and semantics before a cache hit.
            const SceneAssetFileResult header =
                request.forceRecook ? SceneAssetFileResult{} : ReadSceneAssetMetadata(cachePath, request.control.Get());
            if (header.status == SceneImportStatus::Success && SameLookup(header.metadata, lookup)
                && header.metadata.cacheKey == CacheKey(header.metadata)
                && InputsCurrent(header.metadata.inputs, request.control.Get(), context.processing))
            {
                SceneAssetFileResult cached = ReadSceneAssetFile(cachePath, readLimits, request.control.Get(), runtimeOwner);
                if (cached.asset && cached.metadata.cacheKey == header.metadata.cacheKey)
                {
                    result                 = std::move(static_cast<SceneImportResult&>(cached));
                    result.format          = std::string(format->id);
                    result.importerVersion = format->importerVersion;
                    cacheHit               = true;
                    LOGI("Scene source cache hit: {}", request.path);
                    // Eviction removes the least recently used files first.
                    std::error_code touchError;
                    std::filesystem::last_write_time(std::filesystem::u8path(cachePath),
                                                     std::filesystem::file_time_type::clock::now(), touchError);
                }
            }
        }
    }
    if (validation.Succeeded() && !cacheHit)
    {
        LOGI("Scene source import: {}", request.path);
        ProcessedSourceResult imported = ImportProcessedSource(request, context, std::move(previous));
        result                         = std::move(imported.imported);
        if (runtimeOwner != nullptr)
        {
            *runtimeOwner = std::move(imported.runtimeOwner);
        }
        if (result.asset && !cachePath.empty() && (!request.control || !request.control->IsCancelled()))
        {
            SceneAssetFileMetadata metadata  = lookup;
            metadata.inputs.front().size     = sourceStamp.size;
            metadata.inputs.front().modified = sourceStamp.modified;
            metadata.inputs.front().contentHash = imported.processed->sourceContentHash;
            for (const SceneDependency& dependency : result.asset->dependencies)
            {
                metadata.inputs.push_back(dependency);
            }
            metadata.diagnostics   = result.diagnostics;
            const bool runtimeLoad = runtimeOwner != nullptr;
            if ((request.forceRecook || runtimeLoad) && request.control)
            {
                request.control->SetStage(SceneImportStage::SavingCache);
            }
            std::future<SceneImportReport> publication = CacheWriter::Get().Publish(
                {cachePath, std::move(metadata), std::move(imported.processed), request.cacheBudgetBytes, request.control,
                 request.forceRecook, request.directoryCache});
            if (request.forceRecook || runtimeLoad)
            {
                const SceneImportReport written = publication.get();
                if (written.status == SceneImportStatus::Success && runtimeLoad)
                {
                    // Release source image leases and importer geometry before reading the
                    // cache. Runtime scenes retain package descriptors, not the full source
                    // texture set or the background writer's imported scene.
                    result = {};
                    runtimeOwner->Reset();
                    SceneAssetFileResult cached =
                        ReadSceneAssetFile(cachePath, readLimits, request.control.Get(), runtimeOwner);
                    result                      = std::move(static_cast<SceneImportResult&>(cached));
                    result.format               = std::string(format->id);
                    result.importerVersion      = format->importerVersion;
                }
                else if (written.status != SceneImportStatus::Success && request.forceRecook)
                {
                    SetSceneImportFailure(result, written.status, request.path, written.error);
                    for (const SceneImportDiagnostic& diagnostic : written.diagnostics)
                    {
                        result.diagnostics.push_back(diagnostic);
                    }
                }
            }
        }
        else if (result.asset && request.forceRecook && (!request.control || !request.control->IsCancelled()))
        {
            SetSceneImportFailure(result, SceneImportStatus::IOError, request.path,
                                  "Cannot determine a cache destination for recooking");
        }
    }
    if (request.control && request.control->IsCancelled())
    {
        SetSceneImportFailure(result, SceneImportStatus::Cancelled, request.path, "Scene load cancelled");
        cacheHit = false;
    }
    if (!result.asset && runtimeOwner != nullptr)
    {
        runtimeOwner->Reset();
    }
    return result;
}

HeapVector<SceneImportDiagnostic> WaitForSceneAssetCacheWrites()
{
    return CacheWriter::Get().Wait();
}
} // namespace zen::asset
