#pragma once
#include "AssetLib/SceneImporter.h"

namespace zenasset
{
struct Scene;
}

namespace zen::asset
{
// Copies independently owned processor records into engine containers. Only a
// fully converted, validated asset is published. RGBA8, BC7, BC4 and marked BC5
// image views and array-layer storage are supported. The GPU texture loader
// exposes logical layers through texture-owned views of shared native pages.
// The source may be released afterward.
SceneImportResult AdaptProcessedScene(const zenasset::Scene&    source,
                                      const SceneImportControl* control                 = nullptr,
                                      uint64_t                  maxRuntimeGeometryBytes = 16ull * 1024 * 1024 * 1024);
} // namespace zen::asset
