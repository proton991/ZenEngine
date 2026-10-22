#pragma once
#include "Editor/Model/SceneLoadState.h"

namespace zen::editor
{
enum class SceneLoadingAction
{
    None,
    Dismiss,
    Cancel
};
// Reports a UI action; the controller owns cancellation and candidate lifetime.
SceneLoadingAction DrawSceneLoadingDialog(const SceneLoadState& state, const std::string& error);
} // namespace zen::editor
