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
void ServiceSentMessages()
{
    // PeekMessage dispatches sent messages itself. Never remove or dispatch posted
    // messages here, including WM_QUIT, input and application events. Keep the peek
    // unfiltered: PM_QS_SENDMESSAGE-only peeks make Windows report the thread as hung.
    MSG message{};

    PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
}

void WaitForRHIObject(HANDLE handle)
{
    // The wait wakes only for messages that arrive after the queue was last examined.
    ServiceSentMessages();

    bool completed = false;

    while (!completed)
    {
        // Waiting on QS_SENDMESSAGE alone makes Windows report the thread as hung after
        // five seconds, even if it pumps elsewhere; DWM then ghosts its windows and
        // takes their input. Without MWMO_INPUTAVAILABLE, posted messages left queued
        // above do not wake this wait again.
        const DWORD result = MsgWaitForMultipleObjectsEx(1, &handle, INFINITE, QS_ALLINPUT, 0);

        if (result == WAIT_OBJECT_0)
        {
            completed = true;
        }
        else if (result == WAIT_OBJECT_0 + 1)
        {
            ServiceSentMessages();
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
    VERIFY_EXPR_MSG(!m_worker.joinable(), "The RHI thread is already running");

    m_capacity      = std::max(size_t(1), capacity);

    m_stopping      = false;

    m_cleanupClosed = false;

    m_taskFailed.store(false, std::memory_order_release);

    m_space.Signal();

    m_threaded.store(mode == RHIExecutionMode::eThreaded, std::memory_order_release);

    if (m_threaded.load(std::memory_order_acquire))
    {
        m_worker = std::thread(&RHIThread::Run, this);
    }
}

void RHIThread::Stop(std::function<void()> finalizer)
{
    if (m_worker.joinable())
    {
        VERIFY_EXPR(s_current != this);

        {
            std::lock_guard<std::mutex> lock(m_mutex);

            m_stopping  = true;

            m_finalizer = std::move(finalizer);

            m_space.Signal();
        }

        m_available.notify_all();
#if defined(ZEN_WIN32)
        WaitForRHIObject(m_worker.native_handle());
#endif
        m_worker.join();
    }
    else if (finalizer)
    {
        Task task{std::move(finalizer), {}, true};

        ExecuteTask(task);
    }

    // Standalone inline backends remain supported after an executor lifetime.
    // Its finalizer must destroy every native resource before returning.
    m_threaded.store(false, std::memory_order_release);
}

bool RHIThread::Dispatch(std::function<void()> task, std::function<void()> cancelled)
{
    return Enqueue({std::move(task), std::move(cancelled), false}, true);
}

bool RHIThread::DispatchCleanup(std::function<void()> task)
{
    return Enqueue({std::move(task), {}, true}, true);
}

bool RHIThread::TryDispatch(std::function<void()> task)
{
    return Enqueue({std::move(task), {}, false}, false);
}

bool RHIThread::Enqueue(Task task, bool waitForSpace)
{
    bool accepted = false;

    if (!m_threaded.load(std::memory_order_acquire) || s_current == this)
    {
        ExecuteTask(task);

        accepted = true;
    }
    else
    {
        std::unique_lock<std::mutex> lock(m_mutex);

        while (waitForSpace && !task.cleanup && !m_stopping && m_tasks.Size() >= m_capacity)
        {
            lock.unlock();

            m_space.Wait();

            lock.lock();
        }

        // Cleanup bypasses capacity so final releases cannot deadlock a draining worker.
        accepted = task.cleanup ? !m_cleanupClosed : !m_stopping && m_tasks.Size() < m_capacity;

        if (accepted)
        {
            m_tasks.Push(task);

            if (m_tasks.Size() >= m_capacity)
            {
                m_space.Reset();
            }
        }

        lock.unlock();

        if (accepted)
        {
            m_available.notify_one();
        }
        else if (task.cancelled)
        {
            // Admission rejection completes the caller's event without a queued job.
            Task cancellation{std::move(task.cancelled), {}, true};

            ExecuteTask(cancellation);
        }
    }

    return accepted;
}

void RHIThread::ExecuteTask(Task& task) noexcept
{
    bool cancelling = false;

    try
    {
        if (task.cleanup || !m_taskFailed.load(std::memory_order_acquire))
        {
            task.execute();
        }
        else if (task.cancelled)
        {
            cancelling = true;

            task.cancelled();
        }
    }
    catch (...)
    {
        m_taskFailed.store(true, std::memory_order_release);

        std::fprintf(stderr, "ZenEngine: exception contained at RHI task boundary\n");

        if (task.cleanup || cancelling)
        {
            VerificationFailure("RHI cleanup or cancellation", __FILE__, __LINE__);
        }

        if (task.cancelled)
        {
            try
            {
                task.cancelled();
            }
            catch (...)
            {
                VerificationFailure("RHI task cancellation", __FILE__, __LINE__);
            }
        }
    }
}

bool RHIThread::HasTaskFailure() const
{
    return m_taskFailed.load(std::memory_order_acquire);
}

void RHIThread::Fence() {}

void RHIThread::Flush()
{
    Invoke(&RHIThread::Fence);
}

bool RHIThread::IsThreaded() const
{
    return m_threaded.load(std::memory_order_acquire);
}

bool RHIThread::IsCurrentThread() const
{
    return !m_threaded.load(std::memory_order_acquire) || s_current == this;
}

void RHIThread::CheckOwnership() const
{
    VERIFY_EXPR_MSG(IsCurrentThread(), "Native RHI work must run on the RHI thread");
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
        Task task;

        {
            std::unique_lock<std::mutex> lock(m_mutex);

            while (m_tasks.Empty() && !m_stopping)
            {
                m_available.wait(lock);
            }

            finished = m_stopping && m_tasks.Empty();

            if (finished)
            {
                m_cleanupClosed = true;

                task            = {std::move(m_finalizer), {}, true};
            }
            else
            {
                std::optional<Task> pending = m_tasks.TryPop();

                task                        = std::move(*pending);

                // Cleanup can exceed capacity. Wake producers only once a slot exists, or a
                // waiting Dispatch would spin on a signaled event while the queue is full.
                if (m_tasks.Size() < m_capacity)
                {
                    m_space.Signal();
                }
            }
        }

        if (task.execute)
        {
            ExecuteTask(task);
        }
    }

    s_current = nullptr;
}
} // namespace zen
