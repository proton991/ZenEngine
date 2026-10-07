#include "Editor/Platform/EditorWindowChrome.h"

namespace zen::editor
{
class EditorWindowChrome::State
{
public:
    explicit State(platform::NativeWindow& window) : window(window) {}

    platform::NativeWindow& window;
    EditorWindowAction      pending{EditorWindowAction::None};
    bool                    enabled{false};
};

EditorWindowChrome::EditorWindowChrome(platform::NativeWindow& window) : m_state(MakeUnique<State>(window)) {}

EditorWindowChrome::~EditorWindowChrome()
{
    if (m_state->enabled)
    {
        m_state->window.SetCustomFrame(false);
    }
}

bool EditorWindowChrome::Initialize()
{
    m_state->enabled = m_state->window.SetCustomFrame(true);

    return m_state->enabled;
}

bool EditorWindowChrome::IsEnabled() const
{
    return m_state->enabled;
}

bool EditorWindowChrome::IsMaximized() const
{
    return m_state->window.IsMaximized();
}

platform::WindowTitleBarLayout EditorWindowChrome::GetTitleBarLayout() const
{
    return m_state->window.GetTitleBarLayout();
}

void EditorWindowChrome::SetTitleBarRegion(const EditorTitleBarRegion& region)
{
    m_state->window.SetTitleBarRegion({region.menuEnd, region.height, region.controlsWidth, region.inputBlocked});
}

void EditorWindowChrome::RequestAction(EditorWindowAction action)
{
    m_state->pending = action;
}

void EditorWindowChrome::ProcessPendingAction()
{
    const EditorWindowAction action = m_state->pending;

    m_state->pending                = EditorWindowAction::None;

    switch (action)
    {
        case EditorWindowAction::Minimize: m_state->window.Minimize(); break;
        case EditorWindowAction::ToggleMaximize:
            if (IsMaximized())
            {
                m_state->window.Restore();
            }
            else
            {
                m_state->window.Maximize();
            }
            break;
        case EditorWindowAction::Close: m_state->window.RequestClose(); break;
        case EditorWindowAction::None: break;
    }
}
} // namespace zen::editor
