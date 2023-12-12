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
#ifdef ZEN_WIN32
    typedef CONDITION_VARIABLE ConditionVariableData;
    ConditionVariable() : m_osConVar() { InitializeConditionVariable(&m_osConVar); }
    ~ConditionVariable() {}
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
#ifdef ZEN_WIN32
inline void ConditionVariable::Wait(zen::Mutex* pMutex, uint32_t milliseconds)
{
    if (pMutex != nullptr)
    {
        SleepConditionVariableCS(&m_osConVar, pMutex->GetMutexData(), milliseconds);
    }
}
inline void ConditionVariable::NotifyOne() { WakeConditionVariable(&m_osConVar); }

inline void ConditionVariable::NotifyAll() { WakeAllConditionVariable(&m_osConVar); }

template <class Predicate>
void ConditionVariable::Wait(Mutex* pMutex, Predicate predicate, uint32_t milliseconds)
{
    while (!predicate()) { Wait(pMutex, milliseconds); }
}
#endif

} // namespace zen