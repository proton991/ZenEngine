#pragma once
#include "Queue.h"
#include <functional>
#include <memory>
#include <thread>
#include <atomic>
#include <vector>
#include <memory>
#include <exception>
#include <future>
#include "Mutex.h"
#include "ConditionVariable.h"
#include "ObjectBase.h"

namespace zen
{
template <class FuncRetType, class... FuncArgs> class ThreadPool
{
public:
    ThreadPool() { this->Init(); }

    explicit ThreadPool(int nThreads)
    {
        this->Init();
        this->Resize(nThreads);
    }

    // the destructor waits for all the functions in the queue to be finished
    ~ThreadPool() { this->Stop(true); }

    // get the number of running threads in the pool
    int GetSize() { return static_cast<int>(m_threads.size()); }

    // number of idle threads
    int GetNIdle() { return m_nWaiting; }

    std::thread& GetThread(int i) { return *m_threads[i]; }

    // change the number of threads in the pool
    // should be called from one thread, otherwise be careful to not interleave, also with this->stop()
    // nThreads must be >= 0
    void Resize(int nThreads)
    {
        if (!m_stop && !m_finished)
        {
            int numOldThreads = static_cast<int>(m_threads.size());
            if (numOldThreads <= nThreads)
            { // if the number of threads is increased
                m_threads.resize(nThreads);
                m_flags.resize(nThreads);

                for (int i = numOldThreads; i < nThreads; ++i)
                {
                    m_flags[i] = std::make_shared<std::atomic<bool>>(false);
                    this->SetThread(i);
                }
            }
            else
            { // the number of threads is decreased
                for (int i = numOldThreads - 1; i >= nThreads; --i)
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
        while (m_q.Pop(_f)) delete _f; // empty the queue
    }

    // pops a functional wrapper to the original function
    std::function<FuncRetType(FuncArgs...)> Pop()
    {
        std::function<FuncRetType(FuncArgs...)>* _f = nullptr;
        m_q.Pop(_f);
        // at return, delete the function even if an exception occurred
        std::unique_ptr<std::function<FuncRetType(FuncArgs...)>> func(_f);
        std::function<FuncRetType(FuncArgs...)>                  f;
        if (_f) f = *_f;
        return f;
    }

    // wait for all computing threads to finish and stop all threads
    // may be called asynchronously to not pause the calling thread while waiting
    // if isWait == true, all the functions in the queue are run, otherwise the queue is cleared without running the functions
    void Stop(bool isWait = false)
    {
        if (!isWait)
        {
            if (m_stop) return;
            m_stop = true;
            for (int i = 0, n = this->GetSize(); i < n; ++i)
            {
                *m_flags[i] = true; // command the threads to stop
            }
            this->ClearQueue(); // empty the queue
        }
        else
        {
            if (m_finished || m_stop) return;
            m_finished = true; // give the waiting threads a command to finish
        }
        {
            LockAuto lock(&m_mutex);
            m_conVar.NotifyAll(); // stop all waiting threads
        }
        for (const auto& t : m_threads)
        { // wait for the computing threads to finish
            if (t->joinable()) t->join();
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

        auto task = std::make_shared<std::packaged_task<ReturnType(Args...)>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        auto _f =
            new std::function<ReturnType(Args...)>([task](Args... args) { (*task)(args...); });
        m_q.Push(_f);
        LockAuto lock(&m_mutex);
        m_conVar.NotifyOne();

        return task->get_future();
    }

    // without parameters
    template <class F> auto Push(F&& f) -> std::future<std::invoke_result_t<F, FuncArgs...>>
    {
        using ReturnType = std::invoke_result_t<F, FuncArgs...>;
        auto task =
            std::make_shared<std::packaged_task<ReturnType(FuncArgs...)>>(std::forward<F>(f));
        auto _f = new std::function<FuncRetType(FuncArgs...)>(
            [task](FuncArgs... args) { (*task)(args...); });
        m_q.Push(_f);
        LockAuto lock(&m_mutex);
        m_conVar.NotifyOne();

        return task->get_future();
    }

private:
    ZEN_NO_COPY_MOVE(ThreadPool)

    void SetThread(int i)
    {
        std::shared_ptr<std::atomic<bool>> flag(m_flags[i]); // a copy of the shared ptr to the flag
        auto f = [this, i, flag /* a copy of the shared ptr to the flag */]() {
            std::atomic<bool>&                       _flag = *flag;
            std::function<FuncRetType(FuncArgs...)>* _f;

            bool isPop = m_q.Pop(_f);
            while (true)
            {
                while (isPop)
                { // if there is anything in the queue
                    std::unique_ptr<std::function<FuncRetType(FuncArgs...)>> func(
                        _f); // at return, delete the function even if an exception occurred
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
                if (!isPop) return;
            }
        };
        m_threads[i] =
            std::make_unique<std::thread>(f); // compiler may not support std::make_unique()
    }

    void Init()
    {
        m_nWaiting = 0;
        m_stop     = false;
        m_finished = false;
    }

    std::vector<std::unique_ptr<std::thread>> m_threads;
    // queue that holds pushed functions
    ThreadSafeQueue<std::function<FuncRetType(FuncArgs...)>*> m_q;
    // per thread status flags
    std::vector<std::shared_ptr<std::atomic<bool>>> m_flags;
    // ThreadPool status flags
    std::atomic<bool> m_finished;
    std::atomic<bool> m_stop;
    std::atomic<int>  m_nWaiting; // how many threads are waiting

    Mutex             m_mutex;
    ConditionVariable m_conVar;
};
} // namespace zen