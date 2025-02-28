#pragma once
#include "Queue.h"
#include <functional>
#include <thread>
#include <atomic>
#include <vector>
#include <exception>
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
public:
    ThreadPool()
    {
        this->Init();
    }

    explicit ThreadPool(uint32_t nThreads)
    {
        this->Init();
        this->Resize(std::min(std::thread::hardware_concurrency(), nThreads));
    }

    // the destructor waits for all the functions in the queue to be finished
    ~ThreadPool()
    {
        this->Stop(true);
    }

    // get the number of running threads in the pool
    auto GetSize()
    {
        return m_threads.size();
    }

    // number of idle threads
    int GetNIdle()
    {
        return m_nWaiting;
    }

    std::thread& GetThread(uint32_t i)
    {
        return *m_threads[i];
    }

    // change the number of threads in the pool
    // should be called from one thread, otherwise be careful to not interleave, also with this->stop()
    // nThreads must be >= 0
    void Resize(uint32_t nThreads)
    {
        if (!m_stop && !m_finished)
        {
            uint32_t numOldThreads = static_cast<uint32_t>(m_threads.size());
            if (numOldThreads <= nThreads)
            { // if the number of threads is increased
                m_threads.resize(nThreads);
                m_flags.resize(nThreads);

                for (uint32_t i = numOldThreads; i < nThreads; ++i)
                {
                    m_flags[i] = MakeShared<std::atomic<bool>>(false);
                    this->SetThread(i);
                }
            }
            else
            { // the number of threads is decreased
                for (uint32_t i = numOldThreads - 1; i >= nThreads; --i)
                {
                    *m_flags[i] = true; // this thread will finish
                    m_threads[i]->detach();
                }
                {
                    // stop the detached threads that were waiting
                    LockAuto lock(&m_mutex);
                    m_conVar.NotifyAll();
                }
                m_threads.resize(nThreads); // safe to delete because the threads are detached
                m_flags.resize(
                    nThreads); // safe to delete because the threads have copies of shared_ptr of the flags, not originals
            }
        }
    }

    // empty the queue
    void ClearQueue()
    {
        std::function<FuncRetType(FuncArgs...)>* _f;
        while (m_q.Pop(_f))
            delete _f; // empty the queue
    }

    // pops a functional wrapper to the original function
    std::function<FuncRetType(FuncArgs...)> Pop()
    {
        std::function<FuncRetType(FuncArgs...)>* _f = nullptr;
        m_q.Pop(_f);
        // at return, delete the function even if an exception occurred
        UniquePtr<std::function<FuncRetType(FuncArgs...)>> func(_f);
        std::function<FuncRetType(FuncArgs...)> f;
        if (_f)
            f = *_f;
        return f;
    }

    // wait for all computing threads to finish and stop all threads
    // may be called asynchronously to not pause the calling thread while waiting
    // if isWait == true, all the functions in the queue are run, otherwise the queue is cleared without running the functions
    void Stop(bool isWait = false)
    {
        if (!isWait)
        {
            if (m_stop)
                return;
            m_stop = true;
            for (uint32_t i = 0, n = this->GetSize(); i < n; ++i)
            {
                *m_flags[i] = true; // command the threads to stop
            }
            this->ClearQueue(); // empty the queue
        }
        else
        {
            if (m_finished || m_stop)
                return;
            m_finished = true; // give the waiting threads a command to finish
        }
        {
            LockAuto lock(&m_mutex);
            m_conVar.NotifyAll(); // stop all waiting threads
        }
        for (const auto& t : m_threads)
        { // wait for the computing threads to finish
            if (t->joinable())
                t->join();
        }
        // if there were no threads in the pool but some functors in the queue, the functors are not deleted by the threads
        // therefore delete them here
        ClearQueue();
        m_threads.clear();
        m_flags.clear();
    }

    // add new work item to the pool
    template <class F, class... Args> auto Push(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>
    {
        using ReturnType = std::invoke_result_t<F, Args...>;

        auto task = MakeShared<std::packaged_task<ReturnType(Args...)>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        auto res = task->get_future();
        auto _f =
            new std::function<ReturnType(Args...)>([task](Args... args) { (*task)(args...); });
        m_q.Push(_f);
        LockAuto lock(&m_mutex);
        m_conVar.NotifyOne();

        return res;
    }

    // without parameters
    template <class F> auto Push(F&& f) -> std::future<std::invoke_result_t<F, FuncArgs...>>
    {
        using ReturnType = std::invoke_result_t<F, FuncArgs...>;
        auto task = MakeShared<std::packaged_task<ReturnType(FuncArgs...)>>(std::forward<F>(f));
        auto res  = task->get_future();
        auto _f   = new std::function<FuncRetType(FuncArgs...)>(
            [task](FuncArgs... args) { (*task)(args...); });
        m_q.Push(_f);
        LockAuto lock(&m_mutex);
        m_conVar.NotifyOne();

        return res;
    }

private:
    ZEN_NO_COPY_MOVE(ThreadPool)

    void SetThread(uint32_t i)
    {
        SharedPtr<std::atomic<bool>> flag(m_flags[i]); // a copy of the shared ptr to the flag
        auto f = [this, i, flag /* a copy of the shared ptr to the flag */]() {
            std::atomic<bool>& _flag = *flag;
            std::function<FuncRetType(FuncArgs...)>* _f;

            bool isPop = m_q.Pop(_f);
            while (true)
            {
                while (isPop) // if there is anything in the queue
                {
                    // at return, delete the function even if an exception occurred
                    UniquePtr<std::function<FuncRetType(FuncArgs...)>> func(_f);
                    (*_f)(i);
                    if (_flag)
                        return; // the thread is wanted to stop, return even if the queue is not empty yet
                    else
                        isPop = m_q.Pop(_f);
                }
                // the queue is empty here, wait for the next command
                LockAuto lock(&m_mutex);
                ++m_nWaiting;
                m_conVar.Wait(&m_mutex, [this, &_f, &isPop, &_flag]() {
                    isPop = m_q.Pop(_f);
                    return isPop || m_finished || _flag;
                });
                --m_nWaiting;
                if (!isPop)
                    return;
            }
        };
        m_threads[i] = MakeUnique<std::thread>(f);
    }

    void Init()
    {
        m_nWaiting = 0;
        m_stop     = false;
        m_finished = false;
    }

    std::vector<UniquePtr<std::thread>> m_threads;
    // queue that holds pushed functions
    ThreadSafeQueue<std::function<FuncRetType(FuncArgs...)>*> m_q;
    // per thread status flags
    std::vector<SharedPtr<std::atomic<bool>>> m_flags;
    // ThreadPool status flags
    std::atomic<bool> m_finished;
    std::atomic<bool> m_stop;
    std::atomic<uint32_t> m_nWaiting; // how many threads are waiting

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