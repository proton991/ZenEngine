#pragma once
#include <queue>
#include <functional>
#include <optional>
#include "Utils/Errors.h"
#include "Utils/Mutex.h"

namespace zen
{
template <class T> class ThreadSafeQueue
{
public:
    void Push(const T& value)
    {
        LockAuto lock(&m_mutex);
        m_q.push(value);
    }

    void Pop()
    {
        LockAuto lock(&m_mutex);
        VERIFY_EXPR(!m_q.empty());
        m_q.pop();
    }

    T Peek()
    {
        LockAuto lock(&m_mutex);
        VERIFY_EXPR(!m_q.empty());
        return m_q.front();
    }

    std::optional<T> TryPop()
    {
        LockAuto lock(&m_mutex);
        if (m_q.empty())
        {
            return std::nullopt;
        }

        T value = m_q.front();
        m_q.pop();
        return value;
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
    void Push(const T& value)
    {
        m_q.push(value);
    }

    void Pop()
    {
        VERIFY_EXPR(!m_q.empty());
        m_q.pop();
    }

    T Peek()
    {
        VERIFY_EXPR(!m_q.empty());
        return m_q.front();
    }

    bool Empty()
    {
        return m_q.empty();
    }

    size_t Size()
    {
        return m_q.size();
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
