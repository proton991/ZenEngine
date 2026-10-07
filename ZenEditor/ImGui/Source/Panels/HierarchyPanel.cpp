#include "Panels/EditorPanels.h"
#include "EditorWidgets.h"
#include <algorithm>

namespace zen::editor
{
namespace
{
class HierarchyPanel final : public EditorPanel
{
public:
    HierarchyPanel() : EditorPanel({"Hierarchy", "Scene Hierarchy", EditorDockArea::Left}) {}

private:
    bool IsIncluded(NodeId id) const
    {
        return m_search[0] == 0 || m_filter.count(id.index) != 0;
    }

    bool HasIncludedChildren(const EditorScene& scene, NodeId id) const
    {
        const HeapVector<NodeId>& children = scene.GetChildren(id);

        return std::any_of(children.begin(), children.end(), [this](NodeId child) { return IsIncluded(child); });
    }

    // Visits open nodes only, so drawing cost follows the expanded part of the tree.
    void DrawNode(EditorContext& context, NodeId id, bool root)
    {
        const EditorScene& scene  = context.editor.GetScene();

        const sg::Node& node      = *scene.Resolve(id);

        const bool children       = HasIncludedChildren(scene, id);

        ImGuiTreeNodeFlags flags  = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick
                                  | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding
                                  | ImGuiTreeNodeFlags_DrawLinesFull;

        flags                    |= children ? 0 : ImGuiTreeNodeFlags_Leaf;

        flags                    |= id == context.editor.GetSelection().GetNode() ? ImGuiTreeNodeFlags_Selected : 0;

        const std::string name    = scene.GetNodeDisplayName(id);

        ImGui::PushID(int(id.index));

        if (m_search[0] != 0)
        {
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        }
        else if (root)
        {
            ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        }

        const ImVec2 start = ImGui::GetCursorScreenPos();

        const float right  = start.x + ImGui::GetContentRegionAvail().x;

        // Leading spaces leave room for the icon drawn over the label.
        const std::string label = fmt::format("     {}{}", node.IsVisible() ? "" : "[hidden] ", name);

        ImGui::PushStyleColor(ImGuiCol_Header, GetEditorPalette().treeSelection);

        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyle().Colors[node.IsVisible() ? ImGuiCol_Text : ImGuiCol_TextDisabled]);

        const bool open = ImGui::TreeNodeEx("Node", flags, "%s", label.c_str());

        ImGui::PopStyleColor(2);

        DrawEditorIcon(children ? EditorIcon::Folder : EditorIcon::Cube,
                       ImVec2(start.x + ImGui::GetTreeNodeToLabelSpacing(),
                              start.y + ImGui::GetStyle().FramePadding.y + 1 * EditorScale()),
                       13 * EditorScale(), ImGui::GetColorU32(ImGuiCol_TextDisabled));

        // Only names cut off by the panel edge need a tooltip.
        if (ImGui::IsItemHovered()
            && start.x + ImGui::GetTreeNodeToLabelSpacing() + ImGui::CalcTextSize(label.c_str()).x > right)
        {
            ImGui::SetTooltip("%s", name.c_str());
        }

        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        {
            context.editor.GetSelection().SelectNode(id);
        }

        if (open)
        {
            for (const NodeId child : scene.GetChildren(id))
            {
                if (IsIncluded(child))
                {
                    DrawNode(context, child, false);
                }
            }

            ImGui::TreePop();
        }

        ImGui::PopID();
    }

    void DrawContents(EditorContext& context) override
    {
        const EditorScene& scene = context.editor.GetScene();

        ImGui::SetNextItemWidth(-FLT_MIN);

        ImGui::InputTextWithHint("##SearchHierarchy", "Search scene...", m_search, sizeof(m_search));

        if (scene.Get() == nullptr)
        {
            DrawHint(FormatOpenSceneHint(context.editor.GetActions(), "to inspect its hierarchy.").c_str());
        }
        else
        {
            if (m_filterGeneration != scene.GetGeneration() || m_filterText != m_search)
            {
                m_filter           = scene.FilterHierarchy(m_search);

                m_filterGeneration = scene.GetGeneration();

                m_filterText       = m_search;
            }

            const size_t count = m_search[0] == 0 ? scene.GetNodeCount() : m_filter.size();

            ImGui::TextDisabled("%zu %s%s", count, count == 1 ? "node" : "nodes", m_search[0] == 0 ? "" : " found");

            for (const NodeId root : scene.GetRoots())
            {
                if (IsIncluded(root))
                {
                    DrawNode(context, root, true);
                }
            }
        }
    }

    char                    m_search[256]{};
    std::string             m_filterText;
    uint64_t                m_filterGeneration{0};
    HashMap<uint32_t, bool> m_filter;
};
} // namespace

UniquePtr<EditorPanel> CreateHierarchyPanel()
{
    return MakeUnique<HierarchyPanel>();
}
} // namespace zen::editor
