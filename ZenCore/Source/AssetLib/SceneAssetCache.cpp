#include "AssetLib/SceneAssetCache.h"
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
        && SceneAssetCacheKey(identity) == SceneAssetCacheKey(lookup);
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
        if (!entryError && name.find(".zenasset.tmp-") != std::string::npos && used < stale)
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
    std::string                                     path;
    SceneAssetFileMetadata                          metadata;
    SharedPtr<const SceneAsset, MultiThreadCounter> asset;
    uint64_t                                        budget{0};
};

// Publishes caches after LoadScene has returned, so serialization never delays a
// load. Pending work is cancelled when the process shuts the writer down.
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

    void Publish(CachePublication publication)
    {
        {
            LockAuto lock(&m_mutex);
            ++m_pending;
        }
        m_worker.Push([this, publication](uint32_t) { Run(publication); });
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
    void Run(const CachePublication& publication)
    {
        SceneAssetFileMetadata metadata = publication.metadata;
        // Hash exactly the revisions the importer consumed: the stamps recorded
        // during import must still match before and after reading each file.
        bool current = true;
        for (SceneAssetFileInput& input : metadata.inputs)
        {
            AssetFileStamp before;
            AssetFileStamp after;
            AssetDigest    digest = 0;
            if (current && input.exists)
            {
                current = StampAssetFile(input.path, before) && before.size == input.size && before.modified == input.modified
                       && HashAssetFile(input.path, digest, &m_shutdown) && StampAssetFile(input.path, after)
                       && after == before;
                input.contentHash = digest;
            }
            else if (current)
            {
                std::error_code error;
                current = !std::filesystem::exists(std::filesystem::u8path(input.path), error);
            }
        }
        SceneImportReport written;
        if (current && !m_shutdown.IsCancelled())
        {
            metadata.cacheKey = SceneAssetCacheKey(metadata);
            std::error_code             error;
            const std::filesystem::path path = std::filesystem::u8path(publication.path);
            std::filesystem::create_directories(path.parent_path(), error);
            written = WriteValidatedSceneAssetFile(publication.path, *publication.asset, metadata, &m_shutdown);
            if (written.status == SceneImportStatus::Success)
            {
                EvictCaches(path.parent_path(), publication.budget, path);
            }
        }
        LockAuto lock(&m_mutex);
        if (current && written.status != SceneImportStatus::Success && written.status != SceneImportStatus::Cancelled)
        {
            const std::string message = "Scene cache could not be saved: " + written.error;
            LOGW("{}", message);
            m_failures.push_back(
                {SceneDiagnosticSeverity::Warning, written.status, metadata.inputs.front().path, publication.path, message});
        }
        --m_pending;
        m_idle.NotifyAll();
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
    if (request.useCache && format != nullptr && (!request.control || !request.control->IsCancelled()))
    {
        if (request.control)
        {
            request.control->SetStage(SceneImportStage::Reading);
        }
        const std::string           source    = AbsoluteUtf8Path(std::filesystem::u8path(request.path));
        const std::filesystem::path directory = CacheDirectory(request);
        if (!source.empty() && !directory.empty() && StampAssetFile(request.path, sourceStamp))
        {
            lookup.sourceFormat     = std::string(format->id);
            lookup.importerVersion  = format->importerVersion;
            lookup.buildFingerprint = ZEN_SCENE_IMPORT_FINGERPRINT;
            lookup.settings         = request.settings;
            lookup.inputs.push_back({source});
            cachePath = PathToUtf8(directory / (SceneAssetCacheKey(lookup) + ".zenasset"));
            // Metadata inspection avoids allocating stale bulk data. The full
            // reader validates checksum, budgets and semantics before a cache hit.
            const SceneAssetFileResult header = ReadSceneAssetMetadata(cachePath, request.control.Get());
            if (header.status == SceneImportStatus::Success && SameLookup(header.metadata, lookup)
                && header.metadata.cacheKey == SceneAssetCacheKey(header.metadata)
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
    if (!cacheHit)
    {
        result = ImportScene(request, context);
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
            CacheWriter::Get().Publish({cachePath, std::move(metadata), result.asset, request.cacheBudgetBytes});
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
