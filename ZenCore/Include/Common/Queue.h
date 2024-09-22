#pragma once
#include <queue>
#include <functional>
#include "Mutex.h"

namespace zen
{
template <class T> class ThreadSafeQueue
{
public:
    bool Push(const T& value)
    {
        LockAuto lock(&m_mutex);
        m_q.push(value);
        return true;
    }

    bool Pop(T& out)
    {
        LockAuto lock(&m_mutex);
        if (m_q.empty())
        {
            return false;
        }
        out = m_q.front();
        m_q.pop();
        return true;
    }

    bool Empty()
    {
        LockAuto lock(&m_mutex);
        return m_q.empty();
    }

private:
    std::queue<T> m_q;
    Mutex m_mutex;
};

template <class T> class Queue
{
public:
    bool Push(const T& value)
    {
        m_q.push(value);
        return true;
    }

    bool Pop(T& out)
    {
        if (m_q.empty())
        {
            return false;
        }
        out = m_q.front();
        m_q.pop();
        return true;
    }

    bool Empty()
    {
        return m_q.empty();
    }

private:
    std::queue<T> m_q;
};

class DeletionQueue
{
public:
    void Enqueue(std::function<void()>&& function)
    {
        m_deletors.push_back(function);
    }

    void Flush()
    {
        for (auto it = m_deletors.rbegin(); it != m_deletors.rend(); ++it)
        {
            (*it)(); //call the function
        }

        m_deletors.clear();
    }

private:
    std::deque<std::function<void()>> m_deletors;
};

} // namespace zen