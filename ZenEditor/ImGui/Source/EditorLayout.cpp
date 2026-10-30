#include "Editor/ImGui/EditorWorkspace.h"
#include "Editor/ImGui/EditorTheme.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>

namespace zen::editor
{
namespace
{
// Reserve a share of large workspaces for tools, with UI-scaled preferred sizes
// keeping labels readable. In small windows each split is capped at 35%.
float DockRatio(float preferred, float minimumRatio, float available)
{
    return std::clamp(preferred * EditorScale() / std::max(available, 1.0f), minimumRatio, 0.35f);
}

void ScaleDockSizes(ImGuiDockNode& node, ImVec2 scale)
{
    node.SizeRef.x *= scale.x;
    node.SizeRef.y *= scale.y;
    for (ImGuiDockNode* child : node.ChildNodes)
    {
        if (child != nullptr)
        {
            ScaleDockSizes(*child, scale);
        }
    }
}
} // namespace

bool HasEditorLayout(unsigned int dockspace)
{
    const ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspace);

    return node != nullptr && node->IsDockSpace() && node->CentralNode != nullptr;
}

void ResizeEditorLayout(unsigned int dockspace, float width, float height)
{
    ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspace);
    if (node != nullptr && node->Size.x > 0 && node->Size.y > 0 && width > 0 && height > 0
        && (node->Size.x != width || node->Size.y != height))
    {
        // ImGui otherwise preserves side/bottom pixel sizes and assigns the entire
        // window-size change to the scene's central node. Keep the user's proportions.
        ScaleDockSizes(*node, ImVec2(width / node->Size.x, height / node->Size.y));
        ImGui::DockBuilderSetNodeSize(dockspace, ImVec2(width, height));
    }
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
    const float rightRatio = DockRatio(320, 0.20f, width);

    ImGuiID right          = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, rightRatio, nullptr, &center);

    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, DockRatio(280, 0.30f, height), nullptr, &center);

    // Left occupies 18% of the whole workspace, rather than of the remaining center.
    const float leftRatio = DockRatio(260, 0.18f / (1 - rightRatio), width * (1 - rightRatio));

    ImGuiID left          = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, leftRatio, nullptr, &center);

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
