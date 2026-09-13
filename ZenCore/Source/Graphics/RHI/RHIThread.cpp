#include "Graphics/RHI/RHIThread.h"
#include "Utils/Errors.h"
#include <algorithm>
#include <stdexcept>
#if defined(ZEN_WIN32)
#    include <Windows.h>
#endif

namespace zen
{
#if defined(ZEN_WIN32)
namespace
{
void WaitForRHIObject(HANDLE handle)
{
    bool completed = false;
    while (!completed)
    {
        const DWORD result =
            MsgWaitForMultipleObjectsEx(1, &handle, INFINITE, QS_SENDMESSAGE, MWMO_INPUTAVAILABLE);
        if (result == WAIT_OBJECT_0)
        {
            completed = true;
        }
        else if (result == WAIT_OBJECT_0 + 1)
        {
            // PeekMessage dispatches sent messages itself. Never remove or dispatch
            // posted messages here, including WM_QUIT, input and application events.
            MSG message{};
            PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
        }
        else
        {
            LOG_ERROR_AND_THROW("RHI object wait failed: {}", GetLastError());
        }
    }
}
} // namespace
#endif

RHIThreadEvent::RHIThreadEvent(bool signaled)
{
#if defined(ZEN_WIN32)
    m_handle = CreateEventW(nullptr, TRUE, signaled ? TRUE : FALSE, nullptr);
    if (m_handle == nullptr)
    {
        LOG_ERROR_AND_THROW("Cannot create RHI event: {}", GetLastError());
    }
#else
    m_signaled = signaled;
#endif
}

RHIThreadEvent::~RHIThreadEvent()
{
#if defined(ZEN_WIN32)
    CloseHandle(m_handle);
#endif
}

void RHIThreadEvent::Signal()
{
#if defined(ZEN_WIN32)
    VERIFY_EXPR(SetEvent(m_handle));
#else
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_signaled = true;
    }
    m_available.notify_all();
#endif
}

void RHIThreadEvent::Reset()
{
#if defined(ZEN_WIN32)
    VERIFY_EXPR(ResetEvent(m_handle));
#else
    std::lock_guard<std::mutex> lock(m_mutex);
    m_signaled = false;
#endif
}

void RHIThreadEvent::Wait() const
{
#if defined(ZEN_WIN32)
    WaitForRHIObject(m_handle);
#else
    std::unique_lock<std::mutex> lock(m_mutex);
    while (!m_signaled)
    {
        m_available.wait(lock);
    }
#endif
}

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
    m_space.Signal();
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
            m_space.Signal();
        }
        m_available.notify_all();
#if defined(ZEN_WIN32)
        WaitForRHIObject(m_worker.native_handle());
#endif
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
            // Servicing sent messages can enter a window procedure. Do not hold
            // the queue mutex while waiting or running that procedure.
            lock.unlock();
            m_space.Wait();
            lock.lock();
        }
        if (m_stopping)
        {
            throw std::runtime_error("Cannot dispatch work to a stopped RHI thread");
        }
        m_tasks.Push(task);
        if (m_tasks.Size() >= m_capacity)
        {
            m_space.Reset();
        }
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
            if (m_tasks.Size() >= m_capacity)
            {
                m_space.Reset();
            }
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
                m_space.Signal();
            }
        }
        if (task)
        {
            task();
        }
    }
    s_current = nullptr;
}
} // namespace zen
