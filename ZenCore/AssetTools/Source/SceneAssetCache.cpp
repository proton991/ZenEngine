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
#include "Utils/ConditionVariable.h"
#include "Utils/Errors.h"
#include "Utils/Mutex.h"
#include "Utils/ThreadPool.h"
#include <algorithm>
#include <chrono>
#include <filesystem>

namespace zen::asset
{
namespace
{
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

// Removes least recently used caches beyond the budget, and temporary files that
// a crashed writer left behind. The file just published is always kept.
void EvictCaches(const std::filesystem::path& directory, uint64_t budget, const std::filesystem::path& keep)
{
    struct CacheFile
    {
        std::filesystem::path           path;
        uint64_t                        size{0};
        std::filesystem::file_time_type used;
    };
    HeapVector<CacheFile>                     files;
    uint64_t                                  total = 0;
    std::error_code                           error;
    const std::filesystem::file_time_type     stale = std::filesystem::file_time_type::clock::now() - std::chrono::hours(1);
    std::filesystem::directory_iterator       entry(directory, error);
    const std::filesystem::directory_iterator end;
    while (!error && entry != end)
    {
        std::error_code                       entryError;
        const std::filesystem::path           path = entry->path();
        const std::filesystem::file_time_type used = entry->last_write_time(entryError);
        const uint64_t                        size = entryError ? 0 : static_cast<uint64_t>(entry->file_size(entryError));
        const std::string                     name = PathToUtf8(path.filename());
        if (!entryError && used < stale
            && (name.find(".zenasset.tmp-") != std::string::npos || name.find(".zenasset.tmp.") != std::string::npos))
        {
            std::filesystem::remove(path, entryError);
        }
        else if (!entryError && path.extension() == ".zenasset")
        {
            files.push_back({path, size, used});
            total += size;
        }
        entry.increment(error);
    }
    std::sort(files.begin(), files.end(), [](const CacheFile& left, const CacheFile& right) { return left.used < right.used; });
    for (const CacheFile& file : files)
    {
        std::error_code removeError;
        if (total > budget && file.path != keep && std::filesystem::remove(file.path, removeError))
        {
            total -= file.size;
        }
    }
}

struct CachePublication
{
    std::string                                       path;
    SceneAssetFileMetadata                            metadata;
    std::shared_ptr<const zenasset::Scene>            asset;
    uint64_t                                          budget{0};
    SharedPtr<SceneImportControl, MultiThreadCounter> control;
    bool                                              forceRecook{false};
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

// Normal opens publish after loading. Explicit recooks await only their own
// publication on the source worker. Shutdown cancels pending writes.
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
        SceneAssetFileMetadata metadata = publication.metadata;
        PublicationControl     control{&m_shutdown, publication.control.Get()};
        // Hash the revisions consumed by the importer before serialization.
        const zenasset::Status freshness = CheckPublicationInputs(metadata, control, true);
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
            options.metadata                   = MakeProcessedMetadata(metadata);
            options.limits.maxStringBytes      = SceneAssetReadLimits{}.maxStringBytes;
            options.compression.transport      = zenasset::TransportCodec::Zstandard;
            options.compression.zstandardLevel = 12;
            options.compression.meshoptimizer  = true;
            PublicationInputs publicationInputs{&metadata, &control};
            if (publication.forceRecook)
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
                EvictCaches(path.parent_path(), publication.budget, path);
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

SceneImportResult ImportSceneWithCache(const SceneImportRequest& request, const SceneImportContext& context, bool& cacheHit)
{
    SceneImportResult result;
    cacheHit                            = false;
    const SceneFormatDescriptor* format = FindSceneImportFormat(request.path);
    SceneAssetFileMetadata       lookup;
    std::string                  cachePath;
    AssetFileStamp               sourceStamp;
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
            // Metadata inspection avoids allocating stale bulk data. The full
            // reader validates checksum, budgets and semantics before a cache hit.
            const SceneAssetFileResult header =
                request.forceRecook ? SceneAssetFileResult{} : ReadSceneAssetMetadata(cachePath, request.control.Get());
            if (header.status == SceneImportStatus::Success && SameLookup(header.metadata, lookup)
                && header.metadata.cacheKey == CacheKey(header.metadata)
                && InputsCurrent(header.metadata.inputs, request.control.Get(), context.processing))
            {
                SceneAssetFileResult cached = ReadSceneAssetFile(cachePath, {}, request.control.Get());
                if (cached.asset && cached.metadata.cacheKey == header.metadata.cacheKey)
                {
                    result                 = std::move(static_cast<SceneImportResult&>(cached));
                    result.format          = std::string(format->id);
                    result.importerVersion = format->importerVersion;
                    cacheHit               = true;
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
        ProcessedSourceResult imported = ImportProcessedSource(request, context);
        result                         = std::move(imported.imported);
        if (result.asset && !cachePath.empty() && (!request.control || !request.control->IsCancelled()))
        {
            SceneAssetFileMetadata metadata  = lookup;
            metadata.inputs.front().size     = sourceStamp.size;
            metadata.inputs.front().modified = sourceStamp.modified;
            for (const SceneDependency& dependency : result.asset->dependencies)
            {
                metadata.inputs.push_back(dependency);
            }
            metadata.diagnostics = result.diagnostics;
            if (request.forceRecook && request.control)
            {
                request.control->SetStage(SceneImportStage::SavingCache);
            }
            std::future<SceneImportReport> publication =
                CacheWriter::Get().Publish({cachePath, std::move(metadata), std::move(imported.processed),
                                            request.cacheBudgetBytes, request.control, request.forceRecook});
            if (request.forceRecook)
            {
                const SceneImportReport written = publication.get();
                if (written.status != SceneImportStatus::Success)
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
    return result;
}

HeapVector<SceneImportDiagnostic> WaitForSceneAssetCacheWrites()
{
    return CacheWriter::Get().Wait();
}
} // namespace zen::asset
