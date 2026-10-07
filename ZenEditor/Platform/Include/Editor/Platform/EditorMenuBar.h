#pragma once
#include "Editor/Model/EditorActions.h"
#include "Utils/UniquePtr.h"

namespace zen::editor
{
// A toolkit-independent snapshot of the workspace's menus and command availability.
struct EditorMenuItem
{
    std::string                id;
    std::string                label;
    EditorShortcut             shortcut;
    std::string                tooltip;
    bool                       enabled{true};
    bool                       checked{false};
    bool                       separator{false};
    HeapVector<EditorMenuItem> children;

    bool operator==(const EditorMenuItem& other) const
    {
        bool equal = id == other.id && label == other.label && shortcut == other.shortcut && tooltip == other.tooltip
                  && enabled == other.enabled && checked == other.checked && separator == other.separator
                  && children.size() == other.children.size();

        for (size_t i = 0; equal && i < children.size(); ++i)
        {
            equal = children[i] == other.children[i];
        }

        return equal;
    }
};

// Owns the macOS system menu bar. Other platforms use their in-window frontend.
// Main-thread callbacks queue commands; the workspace executes them between frames.
class EditorMenuBar
{
public:
    EditorMenuBar();

    ~EditorMenuBar();

    EditorMenuBar(const EditorMenuBar&)            = delete;
    EditorMenuBar& operator=(const EditorMenuBar&) = delete;

    bool Initialize();

    bool IsEnabled() const;

    // Rebuild only when the snapshot changes, preserving the standard application menu.
    void Update(const HeapVector<EditorMenuItem>& menus);

    bool TakeCommand(std::string& id);

private:
    class State;

    UniquePtr<State> m_state;
};
} // namespace zen::editor
