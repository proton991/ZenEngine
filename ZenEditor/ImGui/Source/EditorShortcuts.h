#pragma once
#include "Editor/Model/EditorActions.h"

namespace zen::editor
{
// Shared input translation for the workspace and focused views. Blocks text entry,
// active widgets and popups; labels, bindings and availability come from the registry.
void HandleActionShortcuts(EditorActions& registry, EditorShortcutScope scope, bool focused, bool nativeMenuShortcuts = false);

// The label without a trailing menu ellipsis, for buttons and tooltips.
std::string GetActionCaption(const EditorAction& action);

// "Caption (Shortcut)", followed by the disabled reason on its own line while the
// action is unavailable.
std::string FormatActionTooltip(const EditorAction& action, bool enabled);
} // namespace zen::editor
