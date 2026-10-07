#include "EditorShortcuts.h"
#include "imgui.h"

namespace zen::editor
{
namespace
{
ImGuiKey ToImGuiKey(platform::Key key)
{
    const uint16_t value = uint16_t(key);

    ImGuiKey result      = ImGuiKey_None;

    if (key >= platform::Key::A && key <= platform::Key::Z)
    {
        result = ImGuiKey(ImGuiKey_A + (value - uint16_t(platform::Key::A)));
    }
    else if (key >= platform::Key::Digit0 && key <= platform::Key::Digit9)
    {
        result = ImGuiKey(ImGuiKey_0 + (value - uint16_t(platform::Key::Digit0)));
    }
    else if (key >= platform::Key::F1 && key <= platform::Key::F24)
    {
        result = ImGuiKey(ImGuiKey_F1 + (value - uint16_t(platform::Key::F1)));
    }
    else
    {
        switch (key)
        {
            case platform::Key::Escape: result = ImGuiKey_Escape; break;
            case platform::Key::Enter: result = ImGuiKey_Enter; break;
            case platform::Key::Tab: result = ImGuiKey_Tab; break;
            case platform::Key::Space: result = ImGuiKey_Space; break;
            case platform::Key::Delete: result = ImGuiKey_Delete; break;
            case platform::Key::Insert: result = ImGuiKey_Insert; break;
            case platform::Key::Home: result = ImGuiKey_Home; break;
            case platform::Key::End: result = ImGuiKey_End; break;
            case platform::Key::PageUp: result = ImGuiKey_PageUp; break;
            case platform::Key::PageDown: result = ImGuiKey_PageDown; break;
            case platform::Key::Left: result = ImGuiKey_LeftArrow; break;
            case platform::Key::Right: result = ImGuiKey_RightArrow; break;
            default: break;
        }
    }

    return result;
}

// Modifiers must match exactly, so F does not also fire for Ctrl+F.
bool IsShortcutPressed(EditorShortcut shortcut)
{
    const ImGuiIO& io  = ImGui::GetIO();

    const ImGuiKey key = ToImGuiKey(shortcut.key);

    const uint16_t modifiers =
        (io.KeyCtrl ? uint16_t(platform::KeyModifier::Control) : 0) | (io.KeyShift ? uint16_t(platform::KeyModifier::Shift) : 0)
        | (io.KeyAlt ? uint16_t(platform::KeyModifier::Alt) : 0) | (io.KeySuper ? uint16_t(platform::KeyModifier::Super) : 0);

    return key != ImGuiKey_None && modifiers == shortcut.modifiers && ImGui::IsKeyPressed(key, false);
}

} // namespace

void HandleActionShortcuts(EditorActions& registry, EditorShortcutScope scope, bool focused, bool nativeMenuShortcuts)
{
    const bool blocked = !focused || ImGui::GetIO().WantTextInput || ImGui::IsAnyItemActive()
                      || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);

    EditorShortcut pressed;

    for (const EditorAction& action : registry.GetActions())
    {
        const bool native =
            nativeMenuShortcuts && scope == EditorShortcutScope::Global
            && (action.shortcut.modifiers & (uint16_t(platform::KeyModifier::Control) | uint16_t(platform::KeyModifier::Super)))
                   != 0;

        if (!blocked && !native && action.shortcutScope == scope && pressed.key == platform::Key::Unknown
            && IsShortcutPressed(action.shortcut))
        {
            pressed = action.shortcut;
        }
    }

    if (pressed.key != platform::Key::Unknown)
    {
        registry.ExecuteShortcut(pressed, scope);
    }
}

std::string GetActionCaption(const EditorAction& action)
{
    std::string result = action.label;

    if (result.ends_with("..."))
    {
        result.resize(result.size() - 3);
    }

    return result;
}

std::string FormatActionTooltip(const EditorAction& action, bool enabled)
{
    std::string result         = GetActionCaption(action);

    const std::string shortcut = FormatShortcut(action.shortcut);

    if (!shortcut.empty())
    {
        result += " (" + shortcut + ")";
    }

    if (!enabled && !action.disabledReason.empty())
    {
        result += "\n" + action.disabledReason;
    }

    return result;
}
} // namespace zen::editor
