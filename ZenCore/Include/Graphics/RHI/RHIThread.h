#pragma once

#include <condition_variable>
#include <cstdint>
#include "Templates/Queue.h"
#include <functional>
#include <future>
#include "Utils/SharedPtr.h"
#include "Utils/RefCountPtr.h"
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <atomic>
#include "Utils/Errors.h"
#include "RHIError.h"

namespace zen
{
enum class RHIExecutionMode
{
    eInline,
    eThreaded
};

// Manual-reset event. Windows waits service sent messages required by Vulkan WSI.
// Posted messages stay queued; window callbacks must defer application work.
class RHIThreadEvent : public RefCounted
{
public:
    explicit RHIThreadEvent(bool signaled = false);

    ~RHIThreadEvent() override;

    RHIThreadEvent(const RHIThreadEvent&)            = delete;

    RHIThreadEvent& operator=(const RHIThreadEvent&) = delete;

    void Signal();

    void Reset();

    void Wait() const;

private:
#if defined(ZEN_WIN32)
    void* m_handle{nullptr};
#else
    mutable std::mutex              m_mutex;
    mutable std::condition_variable m_available;
    bool                            m_signaled{false};
#endif
};

// One ordered executor for backend operations. Tasks must never wait for RenderCore
// work; Windows WSI may synchronously send messages to the window's owning thread.
class RHIThread
{
public:
    RHIThread() = default;

    ~RHIThread();

    RHIThread(const RHIThread&)            = delete;

    RHIThread& operator=(const RHIThread&) = delete;

    // Start reopens admission.
    void Start(RHIExecutionMode mode, size_t capacity = 64);

    // A finalizer retires the backend: admission closes when it starts, in both modes, and
    // stays closed until Start or OpenAdmission. Releases from the finalizer still run.
    void Stop(std::function<void()> finalizer = {});

    // Called when a backend starts without an executor, after an earlier backend was retired.
    void OpenAdmission();

    bool Dispatch(std::function<void()> task, std::function<void()> cancelled = {});

    // Cleanup remains admitted during draining, up to the finalizer boundary.
    bool DispatchCleanup(std::function<void()> task);

    // Optional maintenance work can retry later instead of waiting for queue capacity.
    bool TryDispatch(std::function<void()> task);

    RHIJobAdmissionResult DispatchChecked(std::function<void()> task,
                                          std::function<void()> cancelled = {},
                                          bool waitForSpace               = true);

    // Stop normal work and wake producers; ordered cleanup remains admitted.
    void Fail(RHIError error);

    RHIError GetFailure() const;

    bool HasDeviceLoss() const
    {
        return m_deviceLost.load(std::memory_order_acquire);
    }

    void Flush();

    bool IsThreaded() const;

    bool IsCurrentThread() const;

    void CheckOwnership() const;

    bool HasTaskFailure() const;

    bool IsStopping() const
    {
        return m_stopping.load(std::memory_order_acquire);
    }

    template <typename Function, typename... Args>
    std::invoke_result_t<Function, Args...> Invoke(Function&& function, Args&&... args)
    {
        return IsCurrentThread() ? std::invoke(std::forward<Function>(function), std::forward<Args>(args)...)
                                 : InvokeQueued(std::forward<Function>(function), std::forward<Args>(args)...);
    }

    // Normal jobs have a checked cancellation path. Invoke is reserved for ordered
    // backend operations/cleanup whose callers already enforce the terminal gate.
    template <typename Function, typename... Args>
    RHIResult<std::invoke_result_t<Function, Args...>> InvokeChecked(Function&& function, Args&&... args)
    {
        using Value                                                 = std::invoke_result_t<Function, Args...>;
        using Result                                                = RHIResult<Value>;
        SharedPtr<std::promise<Result>, MultiThreadCounter> promise = MakeShared<std::promise<Result>, MultiThreadCounter>();

        std::future<Result> future                                  = promise->get_future();

        RefCountPtr<RHIThreadEvent> completion                      = MakeRefCountPtr<RHIThreadEvent>();

        const RHIJobAdmissionResult admission                       = DispatchChecked(
            [promise, completion,
             work = std::bind_front(std::forward<Function>(function), std::forward<Args>(args)...)]() mutable noexcept {
                if constexpr (std::is_void_v<Value>)
                {
                    work();

                    promise->set_value(Result{});
                }
                else
                {
                    promise->set_value(Result(work()));
                }

                completion->Signal();
            },
            [this, promise, completion] {
                RHIError error = GetFailure();

                if (!error.IsFailure())
                {
                    error = MakeRHIError(RHIErrorCode::eCancelled, "RHI invocation admission closed", __FILE__, __LINE__);
                }

                promise->set_value(Result(error));

                completion->Signal();
            });

        if (admission)
        {
            completion->Wait();
        }

        return future.get();
    }

private:
    template <typename Function, typename... Args>
    std::invoke_result_t<Function, Args...> InvokeQueued(Function&& function, Args&&... args)
    {
        using Result                                                   = std::invoke_result_t<Function, Args...>;
        SharedPtr<std::promise<Result>, MultiThreadCounter> promise    = MakeShared<std::promise<Result>, MultiThreadCounter>();
        std::future<Result>                                 result     = promise->get_future();
        RefCountPtr<RHIThreadEvent>                         completion = MakeRefCountPtr<RHIThreadEvent>();
        const bool                                          accepted   = DispatchCleanup(
            [promise, completion,
             work = std::bind_front(std::forward<Function>(function), std::forward<Args>(args)...)]() mutable noexcept {
                if constexpr (std::is_void_v<Result>)
                {
                    work();

                    promise->set_value();
                }
                else
                {
                    promise->set_value(work());
                }

                completion->Signal();
            });
        VERIFY_EXPR_MSG(accepted, "Synchronous RHI invocation after cleanup admission closed");
        completion->Wait();
        return result.get();
    }

    void Run();

    struct Task
    {
        std::function<void()> execute;
        std::function<void()> cancelled;
        bool                  cleanup{false};
    };

    RHIJobAdmissionResult Enqueue(Task task, bool waitForSpace);

    RHIJobAdmissionResult Push(const Task& task, bool waitForSpace);

    void ExecuteFinalizer(std::function<void()> finalizer);

    void ExecuteTask(Task& task) noexcept;

    static void Fence();

    std::thread                    m_worker;
    mutable std::mutex             m_mutex;
    std::condition_variable        m_available;
    RHIThreadEvent                 m_space{true};
    Queue<Task>                    m_tasks;
    std::function<void()>          m_finalizer;
    size_t                         m_capacity{64};
    std::atomic<bool>              m_stopping{false};
    std::atomic<bool>              m_threaded{false};
    std::atomic<bool>              m_taskFailed{false};
    std::atomic<bool>              m_deviceLost{false};
    RHIError                       m_failure{};
    std::atomic<bool>              m_cleanupClosed{false};
    static thread_local RHIThread* s_current;
};

RHIThread& GetRHIThread();
} // namespace zen
