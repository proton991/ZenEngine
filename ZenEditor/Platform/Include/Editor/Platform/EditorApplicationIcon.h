#pragma once

namespace zen::editor
{
// Set the native application/Dock icon after the window backend creates NSApp.
// Uses the bundled macOS icon, or the source artwork for unbundled IDE launches.
// Returns false if unsupported or the artwork cannot be loaded.
bool InitializeEditorApplicationIcon();
} // namespace zen::editor
