#pragma once
#include "Editor/Model/SceneLoadState.h"

namespace zen::editor
{
// Returns true when the user dismisses a failed load. Loading state is read only.
bool DrawSceneLoadingDialog(const SceneLoadState& state, const std::string& error);
} // namespace zen::editor
