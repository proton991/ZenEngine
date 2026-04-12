#pragma once

#include "Utils/Errors.h"

namespace zen
{
class RefCounted
{
public:
    RefCounted() = default;

    virtual ~RefCounted()
    {
        VERIFY_EXPR(m_counter.GetValue() == 0);
    }

    uint32_t AddRef()
    {
        uint32_t newValue = m_counter.AddRef();
        return newValue;
    }

    uint32_t Release()
    {
        uint32_t newValue = m_counter.Release();
        if (newValue == 0)
        {
            delete this;
        }
        return newValue;
    }

    uint32_t GetRefCount() const
    {
        return m_counter.GetValue();
    }

private:
    class AtomicCounter
    {
    public:
        uint32_t AddRef()
        {
            uint32_t oldValue = m_count.fetch_add(1, std::memory_order_acquire);
            return oldValue + 1;
        }

        uint32_t Release()
        {
            uint32_t oldValue = m_count.fetch_sub(1, std::memory_order_release);
            return oldValue - 1;
        }

        uint32_t GetValue()
        {
            return m_count.load(std::memory_order_relaxed);
        }

    private:
        std::atomic_uint m_count{0};
    };

    mutable AtomicCounter m_counter;
};

/**
 * A smart pointer to an object which implements AddRef/Release.
 */
template <class T> class RefCountPtr
{
public:
    RefCountPtr() : m_pRawPtr(nullptr) {}

    explicit RefCountPtr(T* pPtr, bool addRef = true)
    {
        m_pRawPtr = pPtr;
        if (pPtr && addRef)
        {
            m_pRawPtr->AddRef();
        }
    }

    RefCountPtr(const RefCountPtr& other)
    {
        m_pRawPtr = other.m_pRawPtr;
        if (m_pRawPtr)
        {
            m_pRawPtr->AddRef();
        }
    }

    T* Get() const
    {
        return m_pRawPtr;
    }

    template <class U> explicit RefCountPtr(const RefCountPtr<U>& other)
    {
        m_pRawPtr = static_cast<T*>(other.Get());
        if (m_pRawPtr)
        {
            m_pRawPtr->AddRef();
        }
    }

    RefCountPtr(RefCountPtr&& other) noexcept
    {
        m_pRawPtr       = other.m_pRawPtr;
        other.m_pRawPtr = nullptr;
    }

    template <class U> explicit RefCountPtr(RefCountPtr<U>&& other)
    {
        m_pRawPtr       = static_cast<T*>(other.Get());
        other.pRawPtr = nullptr;
    }

    ~RefCountPtr()
    {
        if (m_pRawPtr)
        {
            m_pRawPtr->Release();
        }
    }

    RefCountPtr& operator=(T* pPtr)
    {
        if (m_pRawPtr != pPtr)
        {
            T* pOldPtr = m_pRawPtr;
            m_pRawPtr  = pPtr;
            if (m_pRawPtr)
            {
                m_pRawPtr->AddRef();
            }
            if (pOldPtr)
            {
                pOldPtr->Release();
            }
        }

        return *this;
    }

    RefCountPtr& operator=(const RefCountPtr& other) // NOLINT(bugprone-unhandled-self-assignment)
    {
        if (m_pRawPtr != other.m_pRawPtr)
        {
            RefCountPtr(other).Swap(*this);
        }
        return *this;
    }

    template <class U> RefCountPtr& operator=(const RefCountPtr<U>& other) noexcept
    {
        RefCountPtr(other).Swap(*this);
        return *this;
    }

    RefCountPtr& operator=(RefCountPtr&& other) noexcept
    {
        if (this != &other)
        {
            T* pOldPtr      = m_pRawPtr;
            m_pRawPtr       = other.m_pRawPtr;
            other.m_pRawPtr = nullptr;
            if (pOldPtr)
            {
                pOldPtr->Release();
            }
        }
        return *this;
    }

    template <class U> RefCountPtr& operator=(RefCountPtr<U>&& other) noexcept
    {
        T* pOldPtr      = m_pRawPtr;
        m_pRawPtr       = other.pRawPtr;
        other.pRawPtr = nullptr;
        if (pOldPtr)
        {
            pOldPtr->Release();
        }
        return *this;
    }

    T* operator->() const
    {
        return m_pRawPtr;
    }

    operator T*() const
    {
        return m_pRawPtr;
    }

    T** operator&() // NOLINT(google-runtime-operator)
    {
        return &m_pRawPtr;
    }

    uint32_t GetRefCount()
    {
        uint32_t count = 0;
        if (m_pRawPtr)
        {
            count = m_pRawPtr->GetRefCount();
            assert(count > 0);
        }
        return count;
    }

    void Swap(RefCountPtr&& other) noexcept
    {
        T* pTmp         = m_pRawPtr;
        m_pRawPtr       = other.m_pRawPtr;
        other.m_pRawPtr = pTmp;
    }

    void Swap(RefCountPtr& other) noexcept
    {
        T* pTmp         = m_pRawPtr;
        m_pRawPtr       = other.m_pRawPtr;
        other.m_pRawPtr = pTmp;
    }

private:
    T* m_pRawPtr;
};

template <class T, class... Args> RefCountPtr<T> MakeRefCountPtr(Args&&... args_)
{
    return RefCountPtr<T>(new T(std::forward<Args>(args_)...));
}
} // namespace zen
