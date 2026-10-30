#include "RuntimeLog.h"
#include <algorithm>

namespace zen::runtime
{
bool RuntimeLog::Open(const std::filesystem::path& path)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    m_file.open(path, std::ios::binary | std::ios::trunc);
    return m_file.good();
}

void RuntimeLog::flush_()
{
    m_file.flush();
}

void RuntimeLog::Clear()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    m_entries.clear();
    m_bytes = 0;
}

void RuntimeLog::sink_it_(const spdlog::details::log_msg& source)
{
    spdlog::memory_buf_t formatted;
    formatter_->format(source, formatted);
    m_file.write(formatted.data(), static_cast<std::streamsize>(formatted.size()));
    if (source.level >= spdlog::level::warn)
    {
        m_file.flush();
    }
    RuntimeLogEntry message;
    message.level = static_cast<int>(source.level);
    message.text.assign(source.payload.data(), std::min(source.payload.size(), size_t(16 * 1024)));
    std::replace(message.text.begin(), message.text.end(), '\0', ' ');
    while (!m_entries.empty() && (m_entries.size() >= 512 || m_bytes + message.text.size() > 512 * 1024))
    {
        ++m_dropped;
        m_bytes -= m_entries.front().text.size();
        m_entries.erase(m_entries.begin());
    }
    m_bytes += message.text.size();
    m_entries.push_back(message);
}

HeapVector<RuntimeLogEntry> RuntimeLog::Snapshot()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return m_entries;
}

uint64_t RuntimeLog::GetDropped()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return m_dropped;
}
} // namespace zen::runtime
