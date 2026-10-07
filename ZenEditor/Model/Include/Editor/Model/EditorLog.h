#pragma once
#include "Templates/HeapVector.h"
#include <mutex>
#include <string>

namespace zen::editor
{
struct EditorLogEntry
{
    int         level{0};
    std::string text;
};

class EditorLog
{
public:
    void Append(int level, const std::string& text);

    void Clear();

    HeapVector<EditorLogEntry> Query(int minimumLevel, const std::string& search) const;

    // Changes whenever entries are appended or cleared, so views can reuse a query.
    uint64_t GetRevision() const;

private:
    mutable std::mutex         m_mutex;
    HeapVector<EditorLogEntry> m_entries;
    uint64_t                   m_revision{0};
};
} // namespace zen::editor
