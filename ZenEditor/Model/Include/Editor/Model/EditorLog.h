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

private:
    mutable std::mutex         m_mutex;
    HeapVector<EditorLogEntry> m_entries;
};
} // namespace zen::editor
