#include "Editor/Model/EditorActions.h"
#include "Utils/Errors.h"

namespace zen::editor
{
namespace
{
constexpr uint16_t kMatchedModifiers = uint16_t(platform::KeyModifier::Shift) | uint16_t(platform::KeyModifier::Control)
                                     | uint16_t(platform::KeyModifier::Alt) | uint16_t(platform::KeyModifier::Super);

std::string GetKeyName(platform::Key key)
{
    const uint16_t value = uint16_t(key);

    std::string result;

    if (key >= platform::Key::A && key <= platform::Key::Z)
    {
        result = std::string(1, char('A' + value - uint16_t(platform::Key::A)));
    }
    else if (key >= platform::Key::Digit0 && key <= platform::Key::Digit9)
    {
        result = std::string(1, char('0' + value - uint16_t(platform::Key::Digit0)));
    }
    else if (key >= platform::Key::F1 && key <= platform::Key::F24)
    {
        result = "F" + std::to_string(value - uint16_t(platform::Key::F1) + 1);
    }
    else
    {
        switch (key)
        {
            case platform::Key::Escape: result = "Esc"; break;
            case platform::Key::Enter: result = "Enter"; break;
            case platform::Key::Tab: result = "Tab"; break;
            case platform::Key::Space: result = "Space"; break;
            case platform::Key::Delete: result = "Delete"; break;
            case platform::Key::Insert: result = "Insert"; break;
            case platform::Key::Home: result = "Home"; break;
            case platform::Key::End: result = "End"; break;
            case platform::Key::PageUp: result = "PageUp"; break;
            case platform::Key::PageDown: result = "PageDown"; break;
            case platform::Key::Left: result = "Left"; break;
            case platform::Key::Right: result = "Right"; break;
            default: result = "Key " + std::to_string(value); break;
        }
    }

    return result;
}
} // namespace

void EditorActions::Register(EditorAction action)
{
    VERIFY_EXPR_MSG_F(Find(action.id) == nullptr, "Editor action '{}' is registered twice", action.id);

    VERIFY_EXPR_MSG_F(bool(action.execute), "Editor action '{}' has no execute callback", action.id);

    m_actions.push_back(std::move(action));
}

const EditorAction* EditorActions::Find(const std::string& id) const
{
    const EditorAction* result = nullptr;

    for (const EditorAction& action : m_actions)
    {
        result = result == nullptr && action.id == id ? &action : result;
    }

    return result;
}

bool EditorActions::IsEnabled(const std::string& id) const
{
    const EditorAction* action = Find(id);

    return action != nullptr && (!action->enabled || action->enabled());
}

bool EditorActions::Execute(const std::string& id)
{
    const bool enabled = IsEnabled(id);

    if (enabled)
    {
        Find(id)->execute();
    }

    return enabled;
}

bool EditorActions::ExecuteShortcut(EditorShortcut shortcut, EditorShortcutScope scope)
{
    const EditorShortcut chord{shortcut.key, uint16_t(shortcut.modifiers & kMatchedModifiers)};

    std::string match;

    for (const EditorAction& action : m_actions)
    {
        if (match.empty() && action.shortcutScope == scope && action.shortcut.key != platform::Key::Unknown
            && action.shortcut == chord && (!action.enabled || action.enabled()))
        {
            match = action.id;
        }
    }

    // Execute after the search: a callback may register or reorder actions.
    return !match.empty() && Execute(match);
}

const HeapVector<EditorAction>& EditorActions::GetActions() const
{
    return m_actions;
}

std::string FormatShortcut(EditorShortcut shortcut)
{
    std::string result;

    if (shortcut.key != platform::Key::Unknown)
    {
        const char* prefixes[]                  = {"Ctrl+", "Shift+", "Alt+", "Super+"};

        const platform::KeyModifier modifiers[] = {platform::KeyModifier::Control, platform::KeyModifier::Shift,
                                                   platform::KeyModifier::Alt, platform::KeyModifier::Super};

        for (uint32_t index = 0; index < 4; ++index)
        {
            result += (shortcut.modifiers & uint16_t(modifiers[index])) != 0 ? prefixes[index] : "";
        }

        result += GetKeyName(shortcut.key);
    }

    return result;
}
} // namespace zen::editor
