#pragma once
#include "imgui.h"

namespace zen::editor
{
enum class EditorIcon
{
    Open,
    Save,
    Undo,
    Redo,
    Play,
    Pause,
    Stop,
    Cube,
    Folder,
    Image,
    Sphere
};

// Colors for custom drawing that ImGui's style does not cover. Panels take colors from
// the style or from here, never from literals.
struct EditorPalette
{
    ImVec4 brand;
    ImU32  icon;
    ImU32  iconStrong;
    ImU32  cardBackground;
    ImU32  swatchHighlight;
    ImVec4 treeSelection;
    ImU32  windowButtonHover;
    ImU32  closeButtonHover;
    ImVec4 axis[3];
    // Text on the axis colors, such as the Scene view's orientation gizmo labels.
    ImU32 axisLabel;
    // Matches the mesh preview renderer's clear color around a letterboxed image.
    ImU32 previewBackground;
    // Status text: problems the user can still work around, and failed operations.
    ImVec4 warning;
    ImVec4 error;
};

const EditorPalette& GetEditorPalette();

void ApplyEditorTheme(float scale);

float EditorScale();

void DrawEditorIcon(EditorIcon icon, ImVec2 origin, float size, ImU32 color);

// The tooltip appears on hover, including while the button is disabled.
bool EditorToolButton(const char* label, EditorIcon icon, bool enabled = true, const char* tooltip = nullptr);

// A square button showing only the icon; the ID names it, the tooltip describes it.
bool EditorIconButton(const char* id, EditorIcon icon, bool enabled = true, const char* tooltip = nullptr);

void EditorToolbarSeparator();
} // namespace zen::editor
