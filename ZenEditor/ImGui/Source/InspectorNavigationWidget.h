#pragma once
#include "Editor/Model/InspectorNavigation.h"

namespace zen::editor
{
// Tabs and history controls; page contents remain the Inspector panel's responsibility.
class InspectorNavigationWidget
{
public:
    void Draw(InspectorNavigation& navigation, EditorActions& registry, const EditorScene& scene, bool focused);

private:
    uint32_t m_displayedTab{UINT32_MAX};
};
} // namespace zen::editor
