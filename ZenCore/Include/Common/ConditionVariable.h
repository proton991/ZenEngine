#pragma once
// clang-format off
#ifdef ZEN_WIN32
#include <windows.h>
#undef WIN32_NO_STATUS
#endif
#include "ObjectBase.h"
// clang-format on
#include "Mutex.h"

#ifndef INF_TIME
#    define INF_TIME 0xFFFFFFFF
#endif

namespace zen
{
class ConditionVariable
{
public:
#if defined(ZEN_WIN32)
    typedef CONDITION_VARIABLE ConditionVariableData;
    ConditionVariable() : m_osConVar()
    {
        InitializeConditionVariable(&m_osConVar);
    }
    ~ConditionVariable() {}
#endif

#if defined(ZEN_MACOS)
    typedef pthread_cond_t ConditionVariableData;

    ConditionVariable()
    {
        pthread_cond_init(&m_osConVar, NULL);
    }
    ~ConditionVariable()
    {
        pthread_cond_destroy(&m_osConVar);
    }
#endif

    template <class Predicate>
    void Wait(Mutex* pMutex, Predicate predicate, uint32_t milliseconds = INF_TIME);

    void NotifyOne();

    void NotifyAll();

private:
    void Wait(Mutex* pMutex, uint32_t milliseconds = INF_TIME);

    ConditionVariableData m_osConVar;
    ZEN_NO_COPY(ConditionVariable)
};

// Win32 Implementation
#if defined(ZEN_WIN32)
inline void ConditionVariable::Wait(zen::Mutex* pMutex, uint32_t milliseconds)
{
    if (pMutex != nullptr)
    {
        SleepConditionVariableCS(&m_osConVar, pMutex->GetMutexData(), milliseconds);
    }
}
inline void ConditionVariable::NotifyOne()
{
    WakeConditionVariable(&m_osConVar);
}

inline void ConditionVariable::NotifyAll()
{
    WakeAllConditionVariable(&m_osConVar);
}

template <class Predicate>
void ConditionVariable::Wait(Mutex* pMutex, Predicate predicate, uint32_t milliseconds)
{
    while (!predicate())
    {
        Wait(pMutex, milliseconds);
    }
}
#endif

#if defined(ZEN_MACOS)
inline void ConditionVariable::Wait(zen::Mutex* pMutex, uint32_t milliseconds)
{
    if (pMutex != nullptr)
    {
        if (milliseconds == INF_TIME)
        {
            pthread_cond_wait(&m_osConVar, pMutex->GetMutexData());
        }
        else
        {
            auto now     = std::chrono::system_clock::now();
            auto timeout = now + std::chrono::milliseconds(milliseconds);
            timespec ts;
            ts.tv_sec = std::chrono::duration_cast<std::chrono::seconds>(timeout.time_since_epoch())
                            .count();
            ts.tv_nsec = (timeout.time_since_epoch().count() % 1000000000) * 1000;

            pthread_cond_timedwait(&m_osConVar, pMutex->GetMutexData(), &ts);
        }
    }
}
inline void ConditionVariable::NotifyOne()
{
    pthread_cond_signal(&m_osConVar);
}

inline void ConditionVariable::NotifyAll()
{
    pthread_cond_broadcast(&m_osConVar);
}

template <class Predicate>
void ConditionVariable::Wait(Mutex* pMutex, Predicate predicate, uint32_t milliseconds)
{
    while (!predicate())
    {
        Wait(pMutex, milliseconds);
    }
}
#endif
} // namespace zen