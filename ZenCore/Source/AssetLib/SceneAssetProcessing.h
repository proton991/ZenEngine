#pragma once
#include "AssetLib/SceneImporter.h"

namespace zen::asset
{
// Shared processing every source backend runs before publishing its asset. Clips
// that cannot play and morph weight lists that do not match their targets are
// dropped with a warning instead of rejecting the scene; the result is then
// validated once. A failure is recorded in result with the invalid area named.
bool ProcessImportedAsset(SceneAsset& asset, SceneImportResult& result, const std::string& source);
} // namespace zen::asset
