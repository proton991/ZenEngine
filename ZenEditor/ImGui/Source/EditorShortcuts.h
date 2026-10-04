#pragma once
#include "Editor/Model/EditorActions.h"

namespace zen::editor
{
// Shared input translation for the workspace and focused views. Blocks text entry,
// active widgets and popups; labels, bindings and availability come from the registry.
void HandleActionShortcuts(EditorActions& registry, EditorShortcutScope scope, bool focused);
} // namespace zen::editor
