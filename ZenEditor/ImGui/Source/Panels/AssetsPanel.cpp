#include "Panels/EditorPanels.h"
#include "EditorWidgets.h"

namespace zen::editor
{
namespace
{
// Tile geometry follows the font, so captions keep their spacing at any UI scale.
struct AssetTileLayout
{
    ImVec2 size;
    float  thumbnail{0.0f};
    float  cardHeight{0.0f};
    float  nameY{0.0f};
    float  detailY{0.0f};
};

AssetTileLayout GetAssetTileLayout(bool grid)
{
    const float scale = EditorScale();

    const float line  = ImGui::GetTextLineHeight();

    AssetTileLayout layout;

    if (grid)
    {
        layout.thumbnail  = 64 * scale;

        layout.cardHeight = layout.thumbnail + 12 * scale;

        layout.nameY      = 2 * scale + layout.cardHeight + 4 * scale;

        layout.detailY    = layout.nameY + line + 2 * scale;

        layout.size       = ImVec2(125 * scale, layout.detailY + line + 2 * scale);
    }
    else
    {
        layout.size      = ImVec2(ImGui::GetContentRegionAvail().x, std::max(24 * scale, line + 8 * scale));

        layout.thumbnail = std::min(18 * scale, layout.size.y - 4 * scale);

        layout.nameY     = 0.5f * (layout.size.y - line);
    }

    return layout;
}

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

    void DrawAsset(EditorContext& context, const SceneAssetItem& item, const AssetTileLayout& layout)
    {
        const ImVec2 size = layout.size;

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
            const float inset = 5 * scale;

            const ImVec2 top(start.x + inset, start.y + 2 * scale);

            const ImVec2 bottom(start.x + size.x - inset, top.y + layout.cardHeight);

            draw.AddRectFilled(top, bottom, GetEditorPalette().cardBackground, 4 * scale);

            DrawAssetThumbnail(
                context, m_previews, item,
                ImVec2(start.x + (size.x - layout.thumbnail) * 0.5f, top.y + (layout.cardHeight - layout.thumbnail) * 0.5f),
                layout.thumbnail);

            draw.AddRect(top, bottom, ImGui::GetColorU32(active ? ImGuiCol_TabSelectedOverline : ImGuiCol_Border), 4 * scale);

            draw.PushClipRect(ImVec2(start.x, bottom.y), ImVec2(start.x + size.x - inset, start.y + size.y), true);

            draw.AddText(ImVec2(start.x + inset, start.y + layout.nameY), ImGui::GetColorU32(ImGuiCol_Text), item.name.c_str());

            draw.AddText(ImVec2(start.x + inset, start.y + layout.detailY), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                         detail.c_str());

            draw.PopClipRect();
        }
        else
        {
            DrawAssetThumbnail(context, m_previews, item,
                               ImVec2(start.x + 3 * scale, start.y + (size.y - layout.thumbnail) * 0.5f), layout.thumbnail);

            const float detailX =
                std::max(start.x + 240 * scale, start.x + size.x - ImGui::CalcTextSize(detail.c_str()).x - 8 * scale);

            draw.PushClipRect(start, ImVec2(detailX - 8 * scale, start.y + size.y), true);

            draw.AddText(ImVec2(start.x + 28 * scale, start.y + layout.nameY), ImGui::GetColorU32(ImGuiCol_Text),
                         item.name.c_str());

            draw.PopClipRect();

            draw.AddText(ImVec2(detailX, start.y + layout.nameY), ImGui::GetColorU32(ImGuiCol_TextDisabled), detail.c_str());
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
            DrawStatusText(GetEditorPalette().error, context.editor.GetError().c_str());
        }

        if (context.editor.GetLoadState().IsActive())
        {
            ImGui::TextWrapped("Loading: %s", context.editor.GetLoadState().path.c_str());
        }

        if (scene == nullptr)
        {
            DrawHint("No scene is open. Its meshes, materials, textures and animations appear here.");

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

                const AssetTileLayout layout            = GetAssetTileLayout(m_grid);

                const float spacing                     = ImGui::GetStyle().ItemSpacing.x;

                const int columns =
                    m_grid ? std::max(1, int((ImGui::GetContentRegionAvail().x + spacing) / (layout.size.x + spacing))) : 1;

                ImGuiListClipper clipper;

                clipper.Begin((int(shown.size()) + columns - 1) / columns, layout.size.y + ImGui::GetStyle().ItemSpacing.y);

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

                            DrawAsset(context, shown[row * columns + column], layout);
                        }
                    }
                }

                if (shown.empty())
                {
                    DrawHint(m_search[0] != 0 ? "No assets match the search." : "This scene has no assets of this kind.");
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
