#include "Graphics/RHI/RHIThread.h"
#include "Utils/Errors.h"
#include <algorithm>
#include <stdexcept>
#if defined(ZEN_WIN32)
#    include <Windows.h>
#endif

namespace zen
{
thread_local RHIThread* RHIThread::s_current = nullptr;

RHIThread& GetRHIThread()
{
    static RHIThread thread;
    return thread;
}

RHIThread::~RHIThread()
{
    Stop();
}

void RHIThread::Start(RHIExecutionMode mode, size_t capacity)
{
    if (m_worker.joinable())
    {
        throw std::logic_error("The RHI thread is already running");
    }
    m_capacity = std::max(size_t(1), capacity);
    m_stopping = false;
    m_threaded = mode == RHIExecutionMode::eThreaded;
    if (m_threaded)
    {
        m_worker = std::thread(&RHIThread::Run, this);
    }
}

void RHIThread::Stop()
{
    if (m_worker.joinable())
    {
        VERIFY_EXPR(s_current != this);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
        }
        m_available.notify_all();
        m_space.notify_all();
        m_worker.join();
    }
    m_threaded = false;
}

void RHIThread::Dispatch(std::function<void()> task)
{
    if (!m_threaded || s_current == this)
    {
        task();
    }
    else
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        while (!m_stopping && m_tasks.Size() >= m_capacity)
        {
            m_space.wait(lock);
        }
        if (m_stopping)
        {
            throw std::runtime_error("Cannot dispatch work to a stopped RHI thread");
        }
        m_tasks.Push(task);
        lock.unlock();
        m_available.notify_one();
    }
}

bool RHIThread::TryDispatch(std::function<void()> task)
{
    bool accepted = false;
    if (!m_threaded || s_current == this)
    {
        task();
        accepted = true;
    }
    else
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_stopping && m_tasks.Size() < m_capacity)
        {
            m_tasks.Push(task);
            accepted = true;
        }
        lock.unlock();
        if (accepted)
        {
            m_available.notify_one();
        }
    }
    return accepted;
}

void RHIThread::Fence() {}

void RHIThread::Flush()
{
    Invoke(&RHIThread::Fence);
}

bool RHIThread::IsThreaded() const
{
    return m_threaded;
}

bool RHIThread::IsCurrentThread() const
{
    return !m_threaded || s_current == this;
}

void RHIThread::CheckOwnership() const
{
    if (!IsCurrentThread())
    {
        throw std::logic_error("Native RHI work must run on the RHI thread");
    }
}

void RHIThread::Run()
{
    s_current = this;
#if defined(ZEN_WIN32)
    SetThreadDescription(GetCurrentThread(), L"RHIThread");
#endif
    bool finished = false;
    while (!finished)
    {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            while (m_tasks.Empty() && !m_stopping)
            {
                m_available.wait(lock);
            }
            finished = m_stopping && m_tasks.Empty();
            if (!finished)
            {
                task = m_tasks.Peek();
                m_tasks.Pop();
            }
        }
        m_space.notify_one();
        if (task)
        {
            task();
        }
    }
    s_current = nullptr;
}
} // namespace zen
