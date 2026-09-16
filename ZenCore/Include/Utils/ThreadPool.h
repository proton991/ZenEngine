#pragma once
#include "Templates/Queue.h"
#include "Templates/HeapVector.h"
#include <functional>
#include <thread>
#include <atomic>
#include <future>
#include "Mutex.h"
#include "ConditionVariable.h"
#include "ObjectBase.h"
#include "UniquePtr.h"
#include "SharedPtr.h"

namespace zen
{
template <class FuncRetType, class... FuncArgs> class ThreadPool
{
    using TaskFunction = std::function<FuncRetType(FuncArgs...)>;
    using StopFlag     = SharedPtr<std::atomic<bool>, MultiThreadCounter>;

public:
    ThreadPool() = default;

    explicit ThreadPool(uint32_t nThreads)
    {
        const uint32_t available = std::max(1u, std::thread::hardware_concurrency());
        Resize(std::min(available, nThreads));
    }

    ~ThreadPool()
    {
        Stop(true);
    }

    size_t GetSize() const
    {
        return m_threads.size();
    }

    uint32_t GetNIdle() const
    {
        return m_nWaiting.load();
    }

    std::thread& GetThread(uint32_t i)
    {
        return *m_threads[i];
    }

    // Resize and Stop must be serialized by the owner, outside worker tasks.
    void Resize(uint32_t nThreads)
    {
        if (!m_stop && !m_finished)
        {
            const uint32_t oldSize = static_cast<uint32_t>(m_threads.size());
            if (oldSize <= nThreads)
            {
                m_threads.resize(nThreads);
                m_flags.resize(nThreads);
                for (uint32_t i = oldSize; i < nThreads; ++i)
                {
                    m_flags[i] = StopFlag(new std::atomic<bool>(false));
                    m_threads[i] =
                        MakeUnique<std::thread>(&ThreadPool::RunThread, this, i, m_flags[i]);
                }
            }
            else
            {
                for (uint32_t i = nThreads; i < oldSize; ++i)
                {
                    *m_flags[i] = true;
                }
                NotifyAll();
                for (uint32_t i = nThreads; i < oldSize; ++i)
                {
                    m_threads[i]->join();
                }
                m_threads.resize(nThreads);
                m_flags.resize(nThreads);
            }
        }
    }

    void ClearQueue()
    {
        while (std::optional<TaskFunction*> popped = m_q.TryPop())
        {
            delete *popped;
        }
    }

    TaskFunction Pop()
    {
        TaskFunction result;
        const std::optional<TaskFunction*> popped = m_q.TryPop();
        if (popped.has_value())
        {
            UniquePtr<TaskFunction> task(*popped);
            if (task)
            {
                result = std::move(*task);
            }
        }
        return result;
    }

    // A graceful stop drains pending work; an immediate stop discards queued work.
    void Stop(bool isWait = false)
    {
        const bool shouldStop = isWait ? !m_finished && !m_stop : !m_stop;
        if (shouldStop)
        {
            if (isWait)
            {
                m_finished = true;
            }
            else
            {
                m_stop = true;
                for (const StopFlag& flag : m_flags)
                {
                    *flag = true;
                }
                ClearQueue();
            }
            NotifyAll();
            for (const UniquePtr<std::thread>& thread : m_threads)
            {
                if (thread->joinable())
                {
                    thread->join();
                }
            }
            ClearQueue();
            m_threads.clear();
            m_flags.clear();
        }
    }

    template <class F, class... Args>
    std::future<std::invoke_result_t<F, Args...>> Push(F&& f, Args&&... args)
    {
        using ReturnType = std::invoke_result_t<F, Args...>;
        SharedPtr<std::packaged_task<ReturnType()>, MultiThreadCounter> task(
            new std::packaged_task<ReturnType()>(
                std::bind(std::forward<F>(f), std::forward<Args>(args)...)));
        std::future<ReturnType> result = task->get_future();
        Enqueue(new TaskFunction([task](FuncArgs...) { (*task)(); }));
        return result;
    }

    template <class F> std::future<std::invoke_result_t<F, FuncArgs...>> Push(F&& f)
    {
        using ReturnType = std::invoke_result_t<F, FuncArgs...>;
        SharedPtr<std::packaged_task<ReturnType(FuncArgs...)>, MultiThreadCounter> task(
            new std::packaged_task<ReturnType(FuncArgs...)>(std::forward<F>(f)));
        std::future<ReturnType> result = task->get_future();
        Enqueue(new TaskFunction([task](FuncArgs... args) { (*task)(args...); }));
        return result;
    }

private:
    ZEN_NO_COPY_MOVE(ThreadPool)

    void Enqueue(TaskFunction* task)
    {
        UniquePtr<TaskFunction> pending(task);
        LockAuto lock(&m_mutex);
        if (m_stop || m_finished)
        {
            throw std::runtime_error("Cannot enqueue work after the thread pool has stopped");
        }
        m_q.Push(task);
        pending.Release();
        m_conVar.NotifyOne();
    }

    void NotifyAll()
    {
        LockAuto lock(&m_mutex);
        m_conVar.NotifyAll();
    }

    bool TryTakeTask(TaskFunction*& task)
    {
        const std::optional<TaskFunction*> popped = m_q.TryPop();
        task                                      = popped.value_or(nullptr);
        return popped.has_value();
    }

    void RunThread(uint32_t index, StopFlag flag)
    {
        bool running = true;
        while (running)
        {
            TaskFunction* nextTask = nullptr;
            {
                LockAuto lock(&m_mutex);
                ++m_nWaiting;
                m_conVar.Wait(&m_mutex, [this, &nextTask, &flag] {
                    return flag->load() || TryTakeTask(nextTask) || m_finished.load();
                });
                --m_nWaiting;
            }
            if (nextTask != nullptr)
            {
                UniquePtr<TaskFunction> task(nextTask);
                (*task)(index);
            }
            running = nextTask != nullptr && !flag->load();
        }
    }

    HeapVector<UniquePtr<std::thread>> m_threads;
    ThreadSafeQueue<TaskFunction*> m_q;
    HeapVector<StopFlag> m_flags;
    std::atomic<bool> m_finished{false};
    std::atomic<bool> m_stop{false};
    std::atomic<uint32_t> m_nWaiting{0};
    Mutex m_mutex;
    ConditionVariable m_conVar;
};

//class ThreadPool
//{
//private:
//    class ThreadWorker
//    {
//    private:
//        int         m_id;
//        ThreadPool* m_pool;
//
//    public:
//        ThreadWorker(ThreadPool* pool, const int id) : m_pool(pool), m_id(id) {}
//
//        void operator()()
//        {
//            std::function<void(int)> func;
//            bool                     dequeued;
//            while (!m_pool->m_shutdown)
//            {
//                {
//                    std::unique_lock<std::mutex> lock(m_pool->m_conditional_mutex);
//                    if (m_pool->m_queue.Empty()) { m_pool->m_conditional_lock.wait(lock); }
//                    dequeued = m_pool->m_queue.Pop(func);
//                }
//                if (dequeued) { func(m_id); }
//            }
//        }
//    };
//
//    bool                                      m_shutdown;
//    ThreadSafeQueue<std::function<void(int)>> m_queue;
//    std::vector<std::thread>                  m_threads;
//    std::mutex                                m_conditional_mutex;
//    std::condition_variable                   m_conditional_lock;
//
//public:
//    ThreadPool(const int n_threads) :
//        m_threads(std::vector<std::thread>(n_threads)), m_shutdown(false)
//    {
//        init();
//    }
//
//    ThreadPool(const ThreadPool&) = delete;
//    ThreadPool(ThreadPool&&)      = delete;
//
//    ThreadPool& operator=(const ThreadPool&) = delete;
//    ThreadPool& operator=(ThreadPool&&)      = delete;
//
//    // Inits thread pool
//    void init()
//    {
//        for (int i = 0; i < m_threads.size(); ++i)
//        {
//            m_threads[i] = std::thread(ThreadWorker(this, i));
//        }
//    }
//
//    // Waits until threads finish their current task and shutdowns the pool
//    void shutdown()
//    {
//        m_shutdown = true;
//        m_conditional_lock.notify_all();
//
//        for (int i = 0; i < m_threads.size(); ++i)
//        {
//            if (m_threads[i].joinable()) { m_threads[i].join(); }
//        }
//    }
//
//    // Submit a function to be executed asynchronously by the pool
//    template <typename F, typename... Args> auto Push(F&& f)
//        -> std::future<std::invoke_result_t<F, int>>
//    {
//        using return_type = std::invoke_result_t<F, int>;
//        auto task_ptr = std::make_shared<std::packaged_task<return_type(int)>>(std::forward<F>(f));
//
//        // Wrap packaged task into void function
//        std::function<void(int)> wrapper_func = [task_ptr](int id) {
//            (*task_ptr)(id);
//        };
//
//        // Enqueue generic wrapper function
//        m_queue.Push(wrapper_func);
//
//        // Wake up one thread if its waiting
//        m_conditional_lock.notify_one();
//
//        // Return future from promise
//        return task_ptr->get_future();
//    }
//};
} // namespace zen
