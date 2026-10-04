#pragma once
#include "Templates/HeapVector.h"
#include <string>

namespace zen::editor
{
// Session preview settings. Empty texturePath honors the scene/engine default.
struct EditorEnvironment
{
    std::string texturePath;
    float       intensity{1.0f};
    float       rotationDegrees{0.0f};
    bool        lighting{true};
    bool        skybox{true};
};

struct EnvironmentTextureItem
{
    std::string label;
    std::string path;
};

// File discovery only; the renderer validates texture contents when selected.
HeapVector<EnvironmentTextureItem> DiscoverEnvironmentTextures(const std::string& directory);

std::string EnvironmentTextureLabel(const std::string& path);
} // namespace zen::editor
