#pragma once
#include "Editor/Services/EditorController.h"
#include "Editor/Model/EditorLog.h"
#include "Editor/Platform/EditorWindowChrome.h"
#include "ImGui/ImGuiRenderer.h"

namespace zen::editor
{
// What the ImGui frontend draws from and acts on. Editor state and commands come from
// the controller; the remaining members are frontend and per-frame values.
struct EditorContext
{
    EditorController&   editor;
    EditorLog&          log;
    ui::ImGuiRenderer&  renderer;
    EditorWindowChrome& windowChrome;
    ui::UITextureHandle appIcon;
    // Without a platform picker the frontend asks for a typed path when Open runs.
    bool  nativeFileDialog{false};
    bool  sceneVisible{false};
    bool  focused{true};
    float seconds{0};
    float frameMs{0};
    // The render service's state, read once per UI frame through GetRenderSnapshot.
    EditorRenderSnapshot renderSnapshot;
    int                  renderSnapshotFrame{-1};
};
} // namespace zen::editor
