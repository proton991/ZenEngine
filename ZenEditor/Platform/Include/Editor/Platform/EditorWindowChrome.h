#pragma once
#include "Utils/UniquePtr.h"

#include "Platform/NativeWindow.h"

namespace zen::editor
{
enum class EditorWindowAction
{
    None,
    Minimize,
    ToggleMaximize,
    Close
};

// Window-local UI coordinates; only the space between menus and buttons is draggable.
struct EditorTitleBarRegion
{
    float menuEnd{0};
    float height{0};
    float controlsWidth{0};
    bool  inputBlocked{false};
};

// Native window behavior is independent of ImGui and the scene renderer.
// Unsupported platforms retain their standard window decoration.
class EditorWindowChrome
{
public:
    explicit EditorWindowChrome(platform::NativeWindow& window);

    ~EditorWindowChrome();

    bool Initialize();

    bool IsEnabled() const;

    bool IsMaximized() const;

    // Where editor content may go in the title row, and whether the editor draws the
    // window controls or the platform keeps its own.
    platform::WindowTitleBarLayout GetTitleBarLayout() const;

    void SetTitleBarRegion(const EditorTitleBarRegion& region);

    void RequestAction(EditorWindowAction action);

    // Apply UI requests between frames, before polling size and framebuffer dimensions.
    void ProcessPendingAction();

private:
    class State;

    UniquePtr<State> m_state;
};
} // namespace zen::editor
