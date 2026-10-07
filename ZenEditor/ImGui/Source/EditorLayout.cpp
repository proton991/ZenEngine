#include "Editor/ImGui/EditorWorkspace.h"
#include "Editor/ImGui/EditorTheme.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>

namespace zen::editor
{
namespace
{
// Docks start at a preferred size in UI-scaled pixels, so panels keep room for their
// labels at high DPI and wide screens give the extra space to the scene. In small
// windows a dock takes at most 35% of the space it splits.
float DockRatio(float preferred, float available)
{
    return std::min(preferred * EditorScale() / std::max(available, 1.0f), 0.35f);
}
} // namespace

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
    const float rightRatio = DockRatio(380, width);

    ImGuiID right          = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, rightRatio, nullptr, &center);

    ImGuiID bottom         = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, DockRatio(240, height), nullptr, &center);

    ImGuiID left =
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, DockRatio(260, width * (1 - rightRatio)), nullptr, &center);

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
