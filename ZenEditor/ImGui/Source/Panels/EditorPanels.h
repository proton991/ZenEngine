#pragma once
// Panel factories. Private to the ImGui frontend; each panel lives in its own file.
#include "Editor/ImGui/EditorPanel.h"

namespace zen::editor
{
UniquePtr<EditorPanel> CreateHierarchyPanel();

UniquePtr<EditorPanel> CreateScenePanel();

UniquePtr<EditorPanel> CreateInspectorPanel();

UniquePtr<EditorPanel> CreateRenderSettingsPanel();

UniquePtr<EditorPanel> CreateCameraSettingsPanel();

UniquePtr<EditorPanel> CreateAssetsPanel();

UniquePtr<EditorPanel> CreateOutputPanel();
} // namespace zen::editor
