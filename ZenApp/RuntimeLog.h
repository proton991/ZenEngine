#pragma once
#include "Runtime/RuntimeLogEntry.h"
#include "Templates/HeapVector.h"
#include <spdlog/sinks/base_sink.h>
#include <mutex>
#include <fstream>
#include <filesystem>

namespace zen::runtime
{
class RuntimeLog final : public spdlog::sinks::base_sink<std::mutex>
{
public:
    void                        Clear();
    bool                        Open(const std::filesystem::path& path);
    HeapVector<RuntimeLogEntry> Snapshot();
    uint64_t                    GetDropped();

protected:
    void sink_it_(const spdlog::details::log_msg& message) override;
    void flush_() override;

private:
    HeapVector<RuntimeLogEntry> m_entries;
    size_t                      m_bytes{0};
    uint64_t                    m_dropped{0};
    std::ofstream               m_file;
};
} // namespace zen::runtime
