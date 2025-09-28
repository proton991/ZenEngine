#pragma once
#include <atomic>

namespace zen
{
class MultiThreadCounter
{
public:
    MultiThreadCounter()
    {
        m_count.store(1, std::memory_order_relaxed);
    }
    inline void Add()
    {
        m_count.fetch_add(1, std::memory_order_relaxed);
    }
    inline void Dec()
    {
        m_count.fetch_sub(1, std::memory_order_relaxed);
    }
    inline bool Release()
    {
        // result is the value before fetch sub
        auto result = m_count.fetch_sub(1, std::memory_order_acq_rel);
        return result == 1;
    }
    inline auto GetValue() const
    {
        return m_count.load();
    }

private:
    std::atomic_uint m_count;
};

class SingleThreadCounter
{
public:
    inline void Add()
    {
        m_count++;
    }
    inline bool Release()
    {
        return --m_count == 0;
    }
    inline auto Dec()
    {
        m_count--;
    }
    inline auto GetValue() const
    {
        return m_count;
    }

private:
    uint32_t m_count = 1;
};
} // namespace zen