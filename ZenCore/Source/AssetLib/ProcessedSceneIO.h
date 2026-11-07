#pragma once
#include "AssetLib/SceneAssetIO.h"
#include "ZenAsset/Format/BuildMetadata.h"
#include "ZenAsset/Reader/Scene.h"

namespace zen::asset
{
// Assembly-only adaptation: geometry stays in the native reader until appended
// directly to the combined streams. This partial result must not be published;
// the caller validates the completed assembly before runtime construction.
SceneImportResult         AdaptProcessedSceneResources(const zenasset::Scene& source, const SceneImportControl* control);
Vertex                    AdaptProcessedVertex(const zenasset::Vertex& source);
SceneImportStatus         AdaptProcessedStatus(zenasset::StatusCode status);
bool                      ProcessedOperationCancelled(void* control);
SceneImportDiagnostic     AdaptProcessedDiagnostic(const zenasset::Diagnostic& source);
zenasset::SourceSettings  MakeProcessedSettings(const SceneImportSettings& source);
zenasset::BuildMetadata   MakeProcessedMetadata(const SceneAssetFileMetadata& source);
SceneAssetFileMetadata    AdaptProcessedMetadata(const zenasset::BuildMetadata& source);
zenasset::SceneReadLimits ProcessedLimits(const SceneAssetReadLimits& source);
SceneAssetFileResult      ReadProcessedSceneFile(const std::string&                         path,
                                                 const SceneAssetReadLimits&                limits,
                                                 SceneImportControl*                        control,
                                                 bool                                       metadataOnly,
                                                 SharedPtr<SceneAsset, MultiThreadCounter>* runtimeOwner = nullptr);
} // namespace zen::asset
