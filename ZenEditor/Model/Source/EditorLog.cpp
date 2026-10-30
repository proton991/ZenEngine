#include "Editor/Model/EditorLog.h"
#include "Editor/Model/EditorText.h"
#include <algorithm>

namespace zen::editor
{
void EditorLog::Append(int level, const std::string& text)
{
    const std::lock_guard<std::mutex> lock(m_mutex);

    if (m_entries.size() == 2048)
    {
        m_entries.erase(m_entries.begin());
    }

    m_entries.push_back({level, text.substr(0, 4096), ++m_sequence});

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

void EditorLog::AppendSince(uint64_t& sequence, HeapVector<EditorLogEntry>& view) const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    const auto   first = std::upper_bound(m_entries.begin(), m_entries.end(), sequence,
                                          [](uint64_t value, const EditorLogEntry& entry) { return value < entry.sequence; });
    const size_t count = static_cast<size_t>(m_entries.end() - first);
    const size_t retained = 2048 - count;
    if (view.size() > retained)
    {
        view.erase(view.begin(), view.end() - retained);
    }
    view.insert(view.end(), first, m_entries.end());
    sequence = m_sequence;
}
} // namespace zen::editor
