#pragma once
#include "AssetLib/SceneAssetIO.h"
#include "ZenAsset/Format/BuildMetadata.h"

namespace zen::asset
{
SceneImportStatus        AdaptProcessedStatus(zenasset::StatusCode status);
bool                     ProcessedOperationCancelled(void* control);
SceneImportDiagnostic    AdaptProcessedDiagnostic(const zenasset::Diagnostic& source);
zenasset::SourceSettings MakeProcessedSettings(const SceneImportSettings& source);
zenasset::BuildMetadata  MakeProcessedMetadata(const SceneAssetFileMetadata& source);
SceneAssetFileMetadata   AdaptProcessedMetadata(const zenasset::BuildMetadata& source);
SceneAssetFileResult     ReadProcessedSceneFile(const std::string&          path,
                                                const SceneAssetReadLimits& limits,
                                                SceneImportControl*         control,
                                                bool                        metadataOnly);
} // namespace zen::asset
