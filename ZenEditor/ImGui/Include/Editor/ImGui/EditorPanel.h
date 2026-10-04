#pragma once
#include "Utils/UniquePtr.h"
#include "Templates/HeapVector.h"
#include <string>

namespace zen::editor
{
struct EditorContext;

enum class EditorDockArea
{
    Left,
    Center,
    Right,
    Bottom
};

// The ID names the window for docking and keys saved visibility, so it must stay
// stable; the title is display text and may change.
struct EditorPanelDesc
{
    const char*    id;
    const char*    title;
    EditorDockArea area{EditorDockArea::Center};
    bool           visibleByDefault{true};
};

// Frontend-only interface. A future toolkit can use its own view lifecycle.
class EditorPanel
{
public:
    explicit EditorPanel(const EditorPanelDesc& desc);

    virtual ~EditorPanel() = default;

    void Draw(EditorContext& context);

    const EditorPanelDesc& GetDesc() const;

    // The visible title followed by "###" and the stable ID.
    const std::string& GetWindowName() const;

    bool visible{true};

protected:
    // Runs even for hidden/collapsed panels so scene-owned UI resources can retire.
    virtual void Synchronize(EditorContext& context);

    // Style variables that apply to the window frame only; returns how many were pushed.
    virtual int PushWindowStyle();

    virtual void DrawContents(EditorContext& context) = 0;

private:
    EditorPanelDesc m_desc;
    std::string     m_windowName;
};

// In default tab order within each dock area.
HeapVector<UniquePtr<EditorPanel>> CreateEditorPanels();
} // namespace zen::editor
