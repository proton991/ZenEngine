#pragma once
#include "AssetLib/SceneImporter.h"
#include <memory>

namespace zenasset
{
struct Scene;
}

namespace zen::asset
{
struct ProcessedSourceResult
{
    SceneImportResult                      imported;
    std::shared_ptr<const zenasset::Scene> processed;
};
// Apply source processing policy before either cache lookup or source parsing.
zenasset::Status ValidateSourceProcessing(const SceneImportRequest& request);
// Keeps the processor's immutable scene alive for package publication without
// converting the engine representation back into the independent format.
ProcessedSourceResult ImportProcessedSource(const SceneImportRequest& request, const SceneImportContext& context);
} // namespace zen::asset
