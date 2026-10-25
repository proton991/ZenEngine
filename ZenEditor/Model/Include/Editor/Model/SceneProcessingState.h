#pragma once
#include "AssetLib/SceneImporter.h"

namespace zen::editor
{
struct SceneProcessingSettings
{
    bool desktop{false};
    // Keep the desktop draft when selecting Reference; its effective request
    // always uses untouched library defaults with all operations disabled.
    zenasset::ProcessingSettings settings;
};

struct SceneProcessingState
{
    SceneProcessingSettings draft;
    SceneProcessingSettings applied;
    // Empty for an unopened scene or an explicit package loaded as stored.
    std::string sourcePath;
    std::string error;
    bool        cacheHit{false};
};

inline void ApplySceneProcessingSettings(const SceneProcessingSettings& settings, asset::SceneImportRequest& request)
{
    request.profile    = settings.desktop ? "desktop" : "reference";
    request.processing = settings.desktop ? settings.settings : zenasset::ProcessingSettings{};
}
} // namespace zen::editor
