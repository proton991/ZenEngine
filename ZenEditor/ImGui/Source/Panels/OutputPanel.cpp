#include "Panels/EditorPanels.h"
#include "EditorWidgets.h"

namespace zen::editor
{
namespace
{
class OutputPanel final : public EditorPanel
{
public:
    OutputPanel() : EditorPanel({"Output", "Output", EditorDockArea::Bottom}) {}

private:
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

        const HeapVector<EditorLogEntry> entries = context.log.Query(m_level == 0 ? 0 : m_level + 1, m_search);

        SameLineIfFits(ImGui::CalcTextSize("Copy").x + ImGui::GetStyle().FramePadding.x * 2);

        if (ImGui::Button("Copy"))
        {
            std::string text;

            for (const EditorLogEntry& entry : entries)
            {
                text += entry.text + "\n";
            }

            ImGui::SetClipboardText(text.c_str());
        }

        ImGui::Separator();

        if (ImGui::BeginChild("LogLines"))
        {
            for (const EditorLogEntry& entry : entries)
            {
                ImGui::TextUnformatted(entry.text.c_str());
            }
        }

        ImGui::EndChild();
    }

    char m_search[256]{};
    int  m_level{0};
};
} // namespace

UniquePtr<EditorPanel> CreateOutputPanel()
{
    return MakeUnique<OutputPanel>();
}
} // namespace zen::editor
