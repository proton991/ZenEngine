#include "InspectorNavigationWidget.h"
#include "EditorShortcuts.h"
#include "imgui.h"

namespace zen::editor
{
namespace
{
std::string TabTitle(const InspectorTab& tab, const EditorScene& scene)
{
    std::string title = "Selection";

    if (tab.id != 0)
    {
        const InspectionTarget target = tab.GetTarget();

        if (target.node.generation != 0)
        {
            title = scene.GetNodeDisplayName(target.node);
        }
        else
        {
            title = scene.GetAssets().Describe(target.asset).name;
        }
    }

    return title;
}

void DrawHistoryButton(EditorActions& registry, const char* id, ImGuiDir direction)
{
    const EditorAction* action = registry.Find(id);

    if (action != nullptr)
    {
        const bool enabled = registry.IsEnabled(id);

        ImGui::BeginDisabled(!enabled);

        if (ImGui::ArrowButton(id, direction))
        {
            registry.Execute(id);
        }

        ImGui::EndDisabled();

        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            const std::string shortcut = FormatShortcut(action->shortcut);

            const std::string tooltip  = !enabled         ? action->disabledReason
                                       : shortcut.empty() ? action->label
                                                          : action->label + " (" + shortcut + ")";

            ImGui::SetTooltip("%s", tooltip.c_str());
        }
    }
}

void DrawHistory(InspectorNavigation& navigation, EditorActions& registry, bool focused)
{
    DrawHistoryButton(registry, actions::InspectorBack, ImGuiDir_Left);

    ImGui::SameLine();

    DrawHistoryButton(registry, actions::InspectorForward, ImGuiDir_Right);

    ImGui::SameLine();

    ImGui::TextDisabled(navigation.GetActiveTab().id == 0 ? "Follows selection" : "Browsing references");

    const bool inspectorFocused = focused && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    HandleActionShortcuts(registry, EditorShortcutScope::Inspector, inspectorFocused);
}
} // namespace

void InspectorNavigationWidget::Draw(InspectorNavigation& navigation,
                                     EditorActions&       registry,
                                     const EditorScene&   scene,
                                     bool                 focused)
{
    navigation.Synchronize();

    const uint32_t requested = navigation.GetActiveTab().id;

    const bool select        = requested != m_displayedTab;

    uint32_t activated       = requested;

    uint32_t closed          = 0;

    if (ImGui::BeginTabBar("InspectorTabs", ImGuiTabBarFlags_FittingPolicyScroll))
    {
        for (const InspectorTab& tab : navigation.GetTabs())
        {
            const std::string title       = TabTitle(tab, scene);

            const std::string label       = title + "###InspectorTab" + std::to_string(tab.id);

            const ImGuiTabItemFlags flags = select && tab.id == requested ? ImGuiTabItemFlags_SetSelected : 0;

            bool open                     = true;

            if (ImGui::BeginTabItem(label.c_str(), tab.id == 0 ? nullptr : &open, flags))
            {
                if (!select)
                {
                    activated = tab.id;
                }

                ImGui::EndTabItem();
            }

            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", title.c_str());
            }

            if (!open)
            {
                closed = tab.id;
            }
        }

        ImGui::EndTabBar();
    }

    navigation.Activate(activated);

    m_displayedTab = activated;

    // Apply after iteration so closing a tab cannot invalidate the tab list.
    navigation.Close(closed);

    DrawHistory(navigation, registry, focused);

    ImGui::Separator();
}
} // namespace zen::editor
