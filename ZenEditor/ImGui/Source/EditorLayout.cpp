#include "Editor/ImGui/EditorWorkspace.h"
#include "imgui.h"
#include "imgui_internal.h"

namespace zen::editor
{
bool HasEditorLayout(unsigned int dockspace)
{
    const ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspace);

    return node != nullptr && node->IsDockSpace() && node->CentralNode != nullptr;
}

// DockBuilder is internal to ImGui. All default construction is isolated here.
void BuildDefaultEditorLayout(unsigned int                              dockspace,
                              float                                     width,
                              float                                     height,
                              const HeapVector<UniquePtr<EditorPanel>>& panels)
{
    ImGui::DockBuilderRemoveNode(dockspace);

    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);

    ImGui::DockBuilderSetNodeSize(dockspace, ImVec2(width, height));

    ImGuiID center = dockspace;

    // The Inspector spans the full height, as in the proposed workspace.
    ImGuiID right  = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.24f, nullptr, &center);

    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.27f, nullptr, &center);

    ImGuiID left   = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.24f, nullptr, &center);

    for (ImGuiID id : {center, right, bottom, left})
    {
        ImGui::DockBuilderGetNode(id)->LocalFlags |= ImGuiDockNodeFlags_NoWindowMenuButton;
    }

    // Indexed by EditorDockArea.
    const ImGuiID areas[] = {left, center, right, bottom};

    for (const UniquePtr<EditorPanel>& panel : panels)
    {
        ImGui::DockBuilderDockWindow(panel->GetWindowName().c_str(), areas[uint32_t(panel->GetDesc().area)]);
    }

    ImGui::DockBuilderFinish(dockspace);
}
} // namespace zen::editor
