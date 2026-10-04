#pragma once
#include "Editor/ImGui/EditorPanel.h"
#include <filesystem>

namespace zen::editor
{
struct EditorContext;

class EditorWorkspace
{
public:
    // The directory holds the toolkit layout file; neutral preferences belong to the model.
    explicit EditorWorkspace(std::filesystem::path settingsDirectory);

    // Applies saved panel visibility, restores the layout and registers layout actions.
    // The workspace must outlive action dispatch.
    void Initialize(EditorContext& context);

    void Draw(EditorContext& context);

    // Writes the toolkit layout and records panel visibility in the editor preferences.
    void Save(EditorContext& context);

    void SetPanelVisible(const char* id, bool visible);

    void ResetLayout();

private:
    void HandleShortcuts(EditorContext& context);

    void DrawMenus(EditorContext& context);

    void DrawRecentFiles(EditorContext& context);

    void DrawToolbar(EditorContext& context);

    void DrawStatusBar(EditorContext& context);

    void DrawDialogs(EditorContext& context);

    HeapVector<UniquePtr<EditorPanel>> m_panels;
    std::filesystem::path              m_settings;
    bool                               m_resetLayout{false};
    char                               m_path[2048]{};
};

bool HasEditorLayout(unsigned int dockspace);

// Docks each panel in its descriptor's area, in panel order.
void BuildDefaultEditorLayout(unsigned int                              dockspace,
                              float                                     width,
                              float                                     height,
                              const HeapVector<UniquePtr<EditorPanel>>& panels);
} // namespace zen::editor
