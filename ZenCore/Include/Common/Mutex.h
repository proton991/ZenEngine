#pragma once
// clang-format off
#if defined( ZEN_WIN32)
#include <windows.h>
#undef WIN32_NO_STATUS
#endif

#if defined(ZEN_MACOS)
#include <pthread.h>
#endif
#include "ObjectBase.h"
// clang-format on

namespace zen
{

class Mutex
{
public:
#if defined(ZEN_WIN32)
    typedef CRITICAL_SECTION MutexData;

    Mutex() : m_osMutex()
    {
        InitializeCriticalSection(&m_osMutex);
    }
    ~Mutex()
    {
        DeleteCriticalSection(&m_osMutex);
    }
#endif

#if defined(ZEN_MACOS)
    typedef pthread_mutex_t MutexData;
    Mutex() : m_osMutex(PTHREAD_MUTEX_INITIALIZER)
    {
        pthread_mutex_init(&m_osMutex, nullptr);
    }
    ~Mutex()
    {
        pthread_mutex_destroy(&m_osMutex);
    }
#endif


    void Lock();

    void UnLock();

    // no wait
    bool TryLock();

    MutexData* GetMutexData()
    {
        return &m_osMutex;
    }

private:
    MutexData m_osMutex;
    ZEN_NO_COPY(Mutex)
};

// RAII style usage of Mutex
class LockAuto
{
public:
    explicit LockAuto(Mutex* mutex) : m_pMutex(mutex)
    {
        m_pMutex->Lock();
    }

    ~LockAuto()
    {
        m_pMutex->UnLock();
    }

    LockAuto() = delete;

private:
    Mutex* m_pMutex;
    ZEN_NO_COPY(LockAuto)
};

// WIN32 Implementation
#if defined(ZEN_WIN32)
inline void Mutex::Lock()
{
    EnterCriticalSection(&m_osMutex);
}

inline void Mutex::UnLock()
{
    LeaveCriticalSection(&m_osMutex);
}

inline bool Mutex::TryLock()
{
    return TryEnterCriticalSection(&m_osMutex);
}
#endif

#if defined(ZEN_MACOS)
inline void Mutex::Lock()
{
    pthread_mutex_lock(&m_osMutex);
}

inline void Mutex::UnLock()
{
    pthread_mutex_unlock(&m_osMutex);
}

inline bool Mutex::TryLock()
{
    return pthread_mutex_trylock(&m_osMutex) == 0;
}
#endif
} // namespace zen