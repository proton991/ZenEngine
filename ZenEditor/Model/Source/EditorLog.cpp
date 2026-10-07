#include "Editor/Model/EditorLog.h"
#include "Editor/Model/EditorText.h"

namespace zen::editor
{
void EditorLog::Append(int level, const std::string& text)
{
    const std::lock_guard<std::mutex> lock(m_mutex);

    if (m_entries.size() == 2048)
    {
        m_entries.erase(m_entries.begin());
    }

    m_entries.push_back({level, text.substr(0, 4096)});

    ++m_revision;
}

void EditorLog::Clear()
{
    const std::lock_guard<std::mutex> lock(m_mutex);

    m_entries.clear();

    ++m_revision;
}

HeapVector<EditorLogEntry> EditorLog::Query(int minimumLevel, const std::string& search) const
{
    const std::lock_guard<std::mutex> lock(m_mutex);

    HeapVector<EditorLogEntry> result;

    for (const EditorLogEntry& entry : m_entries)
    {
        if (entry.level >= minimumLevel && MatchesSearch(entry.text, search))
        {
            result.push_back(entry);
        }
    }

    return result;
}

uint64_t EditorLog::GetRevision() const
{
    const std::lock_guard<std::mutex> lock(m_mutex);

    return m_revision;
}
} // namespace zen::editor
