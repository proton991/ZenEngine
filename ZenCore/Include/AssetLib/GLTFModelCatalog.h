#pragma once

#include <string>
#include "Templates/HeapVector.h"

namespace zen::asset
{
struct GLTFModelCatalogEntry
{
    // Relative UTF-8 path with forward slashes, including the model extension.
    std::string label;
    // Absolute normalized UTF-8 path accepted by FastGLTFLoader.
    std::string path;
};

// Discover files without importing them. Missing or unreadable roots yield an empty
// catalog; unreadable descendants are skipped and directory symlinks are not followed.
HeapVector<GLTFModelCatalogEntry> DiscoverGLTFModels(const std::string& basePath);
} // namespace zen::asset
