#pragma once
#include <atomic>
#include <cstdint>

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
        const unsigned int result = m_count.fetch_sub(1, std::memory_order_acq_rel);
        return result == 1;
    }
    inline uint32_t GetValue() const
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
    inline void Dec()
    {
        m_count--;
    }
    inline uint32_t GetValue() const
    {
        return m_count;
    }

private:
    uint32_t m_count = 1;
};
} // namespace zen