#pragma once
#include "Templates/HeapVector.h"
#include "Graphics/RenderCore/V2/RenderingSettings.h"
#include <string>

namespace zen::editor
{
// Session preview settings. Empty texturePath honors the scene/engine default.
using EditorEnvironment = rc::EnvironmentSettings;

struct EnvironmentTextureItem
{
    std::string label;
    std::string path;
};

// File discovery only; the renderer validates texture contents when selected.
HeapVector<EnvironmentTextureItem> DiscoverEnvironmentTextures(const std::string& directory);

std::string EnvironmentTextureLabel(const std::string& path);
} // namespace zen::editor
