#include "Editor/ImGui/EditorPanel.h"
#include "Panels/EditorPanels.h"
#include "imgui.h"

namespace zen::editor
{
EditorPanel::EditorPanel(const EditorPanelDesc& desc) :
    visible(desc.visibleByDefault), m_desc(desc), m_windowName(std::string(desc.title) + "###" + desc.id)
{}

void EditorPanel::Draw(EditorContext& context)
{
    Synchronize(context);

    if (visible)
    {
        const int styles    = PushWindowStyle();

        const bool expanded = ImGui::Begin(m_windowName.c_str(), &visible);

        ImGui::PopStyleVar(styles);

        if (expanded)
        {
            DrawContents(context);
        }

        ImGui::End();
    }
}

void EditorPanel::Synchronize(EditorContext&) {}

const EditorPanelDesc& EditorPanel::GetDesc() const
{
    return m_desc;
}

const std::string& EditorPanel::GetWindowName() const
{
    return m_windowName;
}

int EditorPanel::PushWindowStyle()
{
    return 0;
}

HeapVector<UniquePtr<EditorPanel>> CreateEditorPanels()
{
    HeapVector<UniquePtr<EditorPanel>> panels;

    panels.push_back(CreateHierarchyPanel());

    panels.push_back(CreateScenePanel());

    panels.push_back(CreateRenderSettingsPanel());

    panels.push_back(CreateInspectorPanel());

    panels.push_back(CreateCameraSettingsPanel());

    panels.push_back(CreateAssetsPanel());

    panels.push_back(CreateOutputPanel());

    return panels;
}
} // namespace zen::editor
