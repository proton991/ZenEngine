#pragma once
#include "Templates/HeapVector.h"
#include "Runtime/RuntimeLogEntry.h"
#include <mutex>
#include <string>

namespace zen::editor
{
using EditorLogEntry = runtime::RuntimeLogEntry;

class EditorLog
{
public:
    void Append(int level, const std::string& text);

    void Clear();

    HeapVector<EditorLogEntry> Query(int minimumLevel, const std::string& search) const;

    // Append only new entries to an independently clearable, bounded view.
    void AppendSince(uint64_t& sequence, HeapVector<EditorLogEntry>& view) const;

    // Changes whenever entries are appended or cleared, so views can reuse a query.
    uint64_t GetRevision() const;

private:
    mutable std::mutex         m_mutex;
    HeapVector<EditorLogEntry> m_entries;
    uint64_t                   m_revision{0};
    uint64_t                   m_sequence{0};
};
} // namespace zen::editor
