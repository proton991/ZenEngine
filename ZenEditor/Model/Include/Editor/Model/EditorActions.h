#pragma once
#include "Platform/InputTypes.h"
#include "Templates/HeapVector.h"
#include <functional>
#include <string>

namespace zen::editor
{
struct EditorShortcut
{
    platform::Key key{platform::Key::Unknown};
    // platform::KeyModifier bits. Only Shift, Control, Alt and Super take part in matching.
    uint16_t modifiers{0};

    bool operator==(const EditorShortcut&) const = default;
};

// Frontends dispatch global shortcuts once and local shortcuts only in the focused view.
enum class EditorShortcutScope
{
    Global,
    Inspector
};

struct EditorAction
{
    std::string    id;
    std::string    label;
    EditorShortcut shortcut;
    // Explains a disabled action, for example the plan step that enables it.
    std::string disabledReason;
    // Null means always enabled.
    std::function<bool()> enabled;
    std::function<void()> execute;
    EditorShortcutScope   shortcutScope{EditorShortcutScope::Global};
};

// Menus, toolbars and shortcuts dispatch through one registry, so each command has one
// label, shortcut and enabled state wherever it appears. Owners register callbacks for
// the state they control; a duplicate ID is a programming error.
class EditorActions
{
public:
    void Register(EditorAction action);

    const EditorAction* Find(const std::string& id) const;

    bool IsEnabled(const std::string& id) const;

    // Returns false for unknown or disabled actions.
    bool Execute(const std::string& id);

    // Matches only this scope; dispatching Global cannot invoke a local view command.
    bool ExecuteShortcut(EditorShortcut shortcut, EditorShortcutScope scope = EditorShortcutScope::Global);

    const HeapVector<EditorAction>& GetActions() const;

private:
    HeapVector<EditorAction> m_actions;
};

// Display text such as "Ctrl+O"; empty when the action has no shortcut.
std::string FormatShortcut(EditorShortcut shortcut);

namespace actions
{
inline constexpr const char* Open             = "file.open";
inline constexpr const char* Save             = "file.save";
inline constexpr const char* Exit             = "file.exit";
inline constexpr const char* Undo             = "edit.undo";
inline constexpr const char* Redo             = "edit.redo";
inline constexpr const char* ResetLayout      = "view.reset_layout";
inline constexpr const char* FrameAll         = "scene.frame_all";
inline constexpr const char* FrameSelection   = "scene.frame_selection";
inline constexpr const char* InspectorBack    = "inspector.back";
inline constexpr const char* InspectorForward = "inspector.forward";
inline constexpr const char* Move             = "tool.move";
inline constexpr const char* Rotate           = "tool.rotate";
inline constexpr const char* Scale            = "tool.scale";
inline constexpr const char* Play             = "preview.play";
inline constexpr const char* Pause            = "preview.pause";
inline constexpr const char* Stop             = "preview.stop";
} // namespace actions
} // namespace zen::editor
