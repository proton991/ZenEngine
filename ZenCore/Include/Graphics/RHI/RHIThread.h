#pragma once

#include <condition_variable>
#include <cstdint>
#include "Templates/Queue.h"
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

namespace zen
{
enum class RHIExecutionMode
{
    eInline,
    eThreaded
};

// Manual-reset event. Windows waits service sent messages required by Vulkan WSI.
// Posted messages stay queued; window callbacks must defer application work.
class RHIThreadEvent
{
public:
    explicit RHIThreadEvent(bool signaled = false);
    ~RHIThreadEvent();
    RHIThreadEvent(const RHIThreadEvent&)            = delete;
    RHIThreadEvent& operator=(const RHIThreadEvent&) = delete;

    void Signal();
    void Reset();
    void Wait() const;

private:
#if defined(ZEN_WIN32)
    void* m_handle{nullptr};
#else
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_available;
    bool m_signaled{false};
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

    void Start(RHIExecutionMode mode, size_t capacity = 64);
    void Stop();
    void Dispatch(std::function<void()> task);
    // Optional maintenance work can retry later instead of waiting for queue capacity.
    bool TryDispatch(std::function<void()> task);
    void Flush();
    bool IsThreaded() const;
    bool IsCurrentThread() const;
    void CheckOwnership() const;

    template <typename Function, typename... Args>
    std::invoke_result_t<Function, Args...> Invoke(Function&& function, Args&&... args)
    {
        using Result = std::invoke_result_t<Function, Args...>;
        std::shared_ptr<std::packaged_task<Result()>> task =
            std::make_shared<std::packaged_task<Result()>>(
                std::bind_front(std::forward<Function>(function), std::forward<Args>(args)...));
        std::future<Result> result = task->get_future();
        std::shared_ptr<RHIThreadEvent> completion = std::make_shared<RHIThreadEvent>();
        Dispatch([task, completion] {
            (*task)();
            completion->Signal();
        });
        completion->Wait();
        return result.get();
    }

private:
    void Run();
    static void Fence();

    std::thread m_worker;
    std::mutex m_mutex;
    std::condition_variable m_available;
    RHIThreadEvent m_space{true};
    Queue<std::function<void()>> m_tasks;
    size_t m_capacity{64};
    bool m_stopping{false};
    bool m_threaded{false};
    static thread_local RHIThread* s_current;
};

RHIThread& GetRHIThread();
} // namespace zen
