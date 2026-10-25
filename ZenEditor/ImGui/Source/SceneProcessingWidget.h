#pragma once
#include "Editor/Model/SceneProcessingState.h"

namespace zen::editor
{
// Edits a draft only. The controller validates and snapshots it for each request.
bool DrawSceneProcessingSettings(SceneProcessingSettings& settings);
} // namespace zen::editor
