#include "Panels/EditorPanels.h"
#include "EditorWidgets.h"

namespace zen::editor
{
namespace
{
// Resources of the open scene. Opening files belongs to File > Open.
class AssetsPanel final : public EditorPanel
{
public:
    AssetsPanel() : EditorPanel({"Assets", "Assets", EditorDockArea::Bottom}) {}

private:
    void Synchronize(EditorContext& context) override
    {
        m_previews.Synchronize(context);
    }

    void DrawAsset(EditorContext& context, const SceneAssetItem& item, ImVec2 size)
    {
        ImGui::PushID(int(item.id.kind));

        ImGui::PushID(int(item.id.index));

        const bool active  = context.editor.GetSelection().GetAsset() == item.id;

        const ImVec2 start = ImGui::GetCursorScreenPos();

        if (ImGui::Selectable("##Asset", active, ImGuiSelectableFlags_None, size))
        {
            context.editor.GetSelection().SelectAsset(item.id);
        }

        const std::string detail = GetAssetDetail(item);

        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGui::SetTooltip("%s\n%s | %s", item.name.c_str(), GetAssetKindName(item.id.kind, false), detail.c_str());
        }

        ImDrawList& draw  = *ImGui::GetWindowDrawList();

        const float scale = EditorScale();

        if (m_grid)
        {
            const ImVec2 top(start.x + 5 * scale, start.y + 4 * scale);

            const ImVec2 bottom(start.x + size.x - 5 * scale, start.y + 85 * scale);

            draw.AddRectFilled(top, bottom, GetEditorPalette().cardBackground, 4 * scale);

            DrawAssetThumbnail(context, m_previews, item, ImVec2(start.x + (size.x - 70 * scale) * 0.5f, start.y + 10 * scale),
                               70 * scale);

            draw.AddRect(top, bottom, ImGui::GetColorU32(active ? ImGuiCol_TabSelectedOverline : ImGuiCol_Border), 4 * scale);

            draw.PushClipRect(ImVec2(start.x, start.y + 87 * scale), ImVec2(start.x + size.x - 4 * scale, start.y + size.y),
                              true);

            draw.AddText(ImVec2(start.x + 5 * scale, start.y + 90 * scale), ImGui::GetColorU32(ImGuiCol_Text),
                         item.name.c_str());

            draw.AddText(ImVec2(start.x + 5 * scale, start.y + 110 * scale), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                         detail.c_str());

            draw.PopClipRect();
        }
        else
        {
            DrawAssetThumbnail(context, m_previews, item, ImVec2(start.x + 3 * scale, start.y + 2 * scale), 18 * scale);

            const float detailX =
                std::max(start.x + 240 * scale, start.x + size.x - ImGui::CalcTextSize(detail.c_str()).x - 8 * scale);

            draw.PushClipRect(start, ImVec2(detailX - 8 * scale, start.y + size.y), true);

            draw.AddText(ImVec2(start.x + 28 * scale, start.y + 3 * scale), ImGui::GetColorU32(ImGuiCol_Text),
                         item.name.c_str());

            draw.PopClipRect();

            draw.AddText(ImVec2(detailX, start.y + 3 * scale), ImGui::GetColorU32(ImGuiCol_TextDisabled), detail.c_str());
        }

        ImGui::PopID();

        ImGui::PopID();
    }

    void DrawCategories(const HeapVector<SceneAssetItem>& assets)
    {
        uint32_t counts[4] = {0, 0, 0, 0};

        for (const SceneAssetItem& item : assets)
        {
            ++counts[uint32_t(item.id.kind)];
        }

        ImGui::TextDisabled("SCENE ASSETS");

        ImGui::Spacing();

        const std::string all = fmt::format("All ({})", assets.size());

        if (ImGui::Selectable(all.c_str(), m_filter < 0))
        {
            m_filter = -1;
        }

        for (int kind = 0; kind < 4; ++kind)
        {
            const std::string label = fmt::format("{} ({})", GetAssetKindName(SceneAssetKind(kind), true), counts[kind]);

            if (ImGui::Selectable(label.c_str(), m_filter == kind))
            {
                m_filter = kind;
            }
        }
    }

    // Re-queries only when the scene, search text or category changes.
    void UpdateQuery(const EditorScene& scene)
    {
        if (m_queryGeneration != scene.GetGeneration() || m_queryText != m_search || m_queryFilter != m_filter)
        {
            m_matches = scene.GetAssets().Query(m_search);

            m_shown.clear();

            for (const SceneAssetItem& item : m_matches)
            {
                if (m_filter < 0 || int(item.id.kind) == m_filter)
                {
                    m_shown.push_back(item);
                }
            }

            m_queryGeneration = scene.GetGeneration();

            m_queryText       = m_search;

            m_queryFilter     = m_filter;
        }
    }

    void DrawContents(EditorContext& context) override
    {
        const float scale        = EditorScale();

        const LoadedScene* scene = context.editor.GetScene().Get();

        const std::string title =
            scene == nullptr ? "Scene assets" : "Scene assets / " + PathToUtf8(std::filesystem::u8path(scene->path).filename());

        ImGui::AlignTextToFramePadding();

        ImGui::TextUnformatted(title.c_str());

        if (scene != nullptr && ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", scene->path.c_str());
        }

        const float searchWidth = 230 * scale;

        const float toggleWidth = ImGui::CalcTextSize(m_grid ? "List" : "Grid").x + ImGui::GetStyle().FramePadding.x * 2;

        SameLineIfFits(searchWidth + ImGui::GetStyle().ItemSpacing.x + toggleWidth);

        ImGui::SetNextItemWidth(EditorControlWidth(230));

        ImGui::InputTextWithHint("##SearchAssets", "Search assets...", m_search, sizeof(m_search));

        SameLineIfFits(toggleWidth);

        if (ImGui::Button(m_grid ? "List" : "Grid"))
        {
            m_grid = !m_grid;
        }

        ImGui::Separator();

        if (!context.editor.GetError().empty())
        {
            ImGui::TextWrapped("%s", context.editor.GetError().c_str());
        }

        if (context.editor.GetLoadState().IsActive())
        {
            ImGui::TextWrapped("Loading: %s", context.editor.GetLoadState().path.c_str());
        }

        if (scene == nullptr)
        {
            ImGui::TextWrapped("No scene is open. Its meshes, materials, textures and animations appear here.");

            if (ImGui::Button("Open..."))
            {
                context.editor.GetActions().Execute(actions::Open);
            }
        }
        else
        {
            UpdateQuery(context.editor.GetScene());

            if (ImGui::BeginChild("AssetCategories", ImVec2(170 * scale, 0), ImGuiChildFlags_Borders))
            {
                DrawCategories(m_matches);
            }

            ImGui::EndChild();

            ImGui::SameLine();

            if (ImGui::BeginChild("AssetCards"))
            {
                const HeapVector<SceneAssetItem>& shown = m_shown;

                const float tileWidth                   = m_grid ? 125 * scale : ImGui::GetContentRegionAvail().x;

                const float tileHeight                  = m_grid ? 132 * scale : 24 * scale;

                const int columns                       = m_grid
                                                            ? std::max(1, int((ImGui::GetContentRegionAvail().x + ImGui::GetStyle().ItemSpacing.x)
                                                        / (tileWidth + ImGui::GetStyle().ItemSpacing.x)))
                                                            : 1;

                ImGuiListClipper clipper;

                clipper.Begin((int(shown.size()) + columns - 1) / columns, tileHeight + ImGui::GetStyle().ItemSpacing.y);

                while (clipper.Step())
                {
                    for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                    {
                        for (int column = 0; column < columns && row * columns + column < int(shown.size()); ++column)
                        {
                            if (column > 0)
                            {
                                ImGui::SameLine();
                            }

                            DrawAsset(context, shown[row * columns + column], ImVec2(tileWidth, tileHeight));
                        }
                    }
                }

                if (shown.empty())
                {
                    ImGui::TextWrapped(m_search[0] != 0 ? "No assets match the search."
                                                        : "This scene has no assets of this kind.");
                }
            }

            ImGui::EndChild();
        }
    }

    PreviewImages              m_previews;
    char                       m_search[256]{};
    int                        m_filter{-1};
    bool                       m_grid{true};
    HeapVector<SceneAssetItem> m_matches;
    HeapVector<SceneAssetItem> m_shown;
    uint64_t                   m_queryGeneration{0};
    std::string                m_queryText;
    int                        m_queryFilter{-1};
};
} // namespace

UniquePtr<EditorPanel> CreateAssetsPanel()
{
    return MakeUnique<AssetsPanel>();
}
} // namespace zen::editor
