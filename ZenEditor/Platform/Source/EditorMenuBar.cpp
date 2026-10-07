#include "Editor/Platform/EditorMenuBar.h"

namespace zen::editor
{
class EditorMenuBar::State
{};

EditorMenuBar::EditorMenuBar() : m_state(MakeUnique<State>()) {}

EditorMenuBar::~EditorMenuBar() = default;

bool EditorMenuBar::Initialize()
{
    return false;
}

bool EditorMenuBar::IsEnabled() const
{
    return false;
}

void EditorMenuBar::Update(const HeapVector<EditorMenuItem>& menus) {}

bool EditorMenuBar::TakeCommand(std::string& id)
{
    return false;
}
} // namespace zen::editor
