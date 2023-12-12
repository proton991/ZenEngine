#pragma once
#include <queue>
#include <mutex>

namespace zen
{
template <class T> class ThreadSafeQueue
{
public:
    bool Push(const T& value)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_q.push(value);
        return true;
    }

    bool Pop(T& out)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_q.empty()) { return false; }
        out = m_q.front();
        m_q.pop();
        return true;
    }

    bool Empty() { return m_q.empty(); }

private:
    std::queue<T> m_q;
    std::mutex    m_mutex;
};
} // namespace zen