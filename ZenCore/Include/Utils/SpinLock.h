#pragma once

#if defined(ZEN_MACOS)
#    include <os/Lock.h>
#else
#    include <atomic>
#endif
namespace zen
{
#if defined(ZEN_MACOS)
class SpinLock
{
    mutable os_unfair_lock m_lock = OS_UNFAIR_LOCK_INIT;

public:
    void Lock() const
    {
        os_unfair_lock_lock(&m_lock);
    }

    void Unlock() const
    {
        os_unfair_lock_unlock(&m_lock);
    }
};
#else
class SpinLock
{
    mutable std::atomic_flag m_locked = ATOMIC_FLAG_INIT;

public:
    void Lock() const
    {
        while (m_locked.test_and_set(std::memory_order_acquire))
        {
            // Continue.
        }
    }
    void Unlock() const
    {
        m_locked.clear(std::memory_order_release);
    }
};
#endif

} // namespace zen