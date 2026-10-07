#include "Panels/EditorPanels.h"
#include "EditorWidgets.h"
#include <algorithm>

namespace zen::editor
{
namespace
{
class OutputPanel final : public EditorPanel
{
public:
    OutputPanel() : EditorPanel({"Output", "Output", EditorDockArea::Bottom}) {}

private:
    // Copies entries only when the log, level or filter changed, not every frame.
    void UpdateQuery(const EditorLog& log)
    {
        const uint64_t revision = log.GetRevision();

        const bool filtered     = m_level != m_queryLevel || m_queryText != m_search;

        if (revision != m_queryRevision || filtered)
        {
            m_entries       = log.Query(m_level == 0 ? 0 : m_level + 1, m_search);

            m_lines         = SplitLogLines(m_entries);

            m_queryRevision = revision;

            m_queryLevel    = m_level;

            m_queryText     = m_search;

            // New entries keep the horizontal range; another filter or a cleared log starts over.
            if (filtered || m_entries.empty())
            {
                m_contentWidth = 0;
            }
        }
    }

    // Log levels follow spdlog: trace, debug, info, warn, err, critical.
    static ImVec4 GetLevelColor(int level)
    {
        const EditorPalette& palette = GetEditorPalette();

        return level >= 4 ? palette.error
             : level == 3 ? palette.warning
             : level <= 1 ? ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]
                          : ImGui::GetStyle().Colors[ImGuiCol_Text];
    }

    void DrawContents(EditorContext& context) override
    {
        ImGui::SetNextItemWidth(EditorControlWidth(240));

        ImGui::InputTextWithHint("##SearchOutput", "Filter output...", m_search, sizeof(m_search));

        SameLineIfFits(100 * EditorScale());

        const char* levels[] = {"All", "Info", "Warning", "Error"};

        ImGui::SetNextItemWidth(EditorControlWidth(100));

        ImGui::Combo("##Level", &m_level, levels, 4);

        SameLineIfFits(ImGui::CalcTextSize("Clear").x + ImGui::GetStyle().FramePadding.x * 2);

        if (ImGui::Button("Clear"))
        {
            context.log.Clear();
        }

        UpdateQuery(context.log);

        SameLineIfFits(ImGui::CalcTextSize("Copy").x + ImGui::GetStyle().FramePadding.x * 2);

        if (ImGui::Button("Copy"))
        {
            std::string text;

            for (const EditorLogEntry& entry : m_entries)
            {
                text += entry.text + "\n";
            }

            ImGui::SetClipboardText(text.c_str());
        }

        ImGui::Separator();

        // Clipping measures only the rows it draws, so the horizontal range is the widest
        // row seen so far rather than following whichever rows are on screen.
        ImGui::SetNextWindowContentSize(ImVec2(m_contentWidth, 0));

        if (ImGui::BeginChild("LogLines", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar))
        {
            // Follow new entries while the view is at the bottom; scrolling up holds it.
            const bool follow = ImGui::GetScrollY() >= ImGui::GetScrollMaxY();

            ImGuiListClipper clipper;

            clipper.Begin(int(m_lines.size()));

            while (clipper.Step())
            {
                for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index)
                {
                    const LogLine& line = m_lines[index];

                    const char* text    = m_entries[line.entry].text.c_str();

                    ImGui::PushStyleColor(ImGuiCol_Text, GetLevelColor(m_entries[line.entry].level));

                    ImGui::TextUnformatted(text + line.begin, text + line.end);

                    ImGui::PopStyleColor();

                    m_contentWidth = std::max(m_contentWidth, ImGui::GetItemRectSize().x);
                }
            }

            if (follow && m_shownRevision != m_queryRevision)
            {
                ImGui::SetScrollHereY(1.0f);
            }

            m_shownRevision = m_queryRevision;
        }

        ImGui::EndChild();
    }

    char                       m_search[256]{};
    int                        m_level{0};
    HeapVector<EditorLogEntry> m_entries;
    HeapVector<LogLine>        m_lines;
    float                      m_contentWidth{0};
    uint64_t                   m_queryRevision{UINT64_MAX};
    uint64_t                   m_shownRevision{UINT64_MAX};
    int                        m_queryLevel{0};
    std::string                m_queryText;
};
} // namespace

UniquePtr<EditorPanel> CreateOutputPanel()
{
    return MakeUnique<OutputPanel>();
}
} // namespace zen::editor
