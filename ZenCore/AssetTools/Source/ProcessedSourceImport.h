#pragma once
#include "AssetLib/SceneImporter.h"
#include <memory>

namespace zenasset
{
struct Scene;
struct SceneImportImageCache;
class Package;
}

namespace zen::asset
{
// Directory publication bounds simultaneous source decodes and independent image writes.
inline constexpr uint32_t DirectoryImageWorkers = 8;

struct ProcessedSourceResult
{
    SceneImportResult                         imported;
    std::shared_ptr<const zenasset::Scene>    processed;
    SharedPtr<SceneAsset, MultiThreadCounter> runtimeOwner;
};
// Apply source processing policy before either cache lookup or source parsing.
zenasset::Status ValidateSourceProcessing(const SceneImportRequest& request);
// Keeps the processor's immutable scene alive for package publication without
// converting the engine representation back into the independent format.
ProcessedSourceResult ImportProcessedSource(const SceneImportRequest&                request,
                                            const SceneImportContext&                context,
                                            std::shared_ptr<const zenasset::Package> previous   = {},
                                            const zenasset::SceneImportImageCache*   imageCache = nullptr);
} // namespace zen::asset
