#pragma once

#include <cassert>

namespace zen
{
/**
 * A smart pointer to an object which implements AddRef/Release.
 */
template <class T> class RefCountPtr
{
public:
    RefCountPtr() : m_rawPtr(nullptr) {}

    explicit RefCountPtr(T* ptr, bool addRef = true)
    {
        m_rawPtr = ptr;
        if (ptr && addRef)
        {
            m_rawPtr->AddRef();
        }
    }

    RefCountPtr(const RefCountPtr& other)
    {
        m_rawPtr = other.m_rawPtr;
        if (m_rawPtr)
        {
            m_rawPtr->AddRef();
        }
    }

    T* Get() const
    {
        return m_rawPtr;
    }

    template <class U> explicit RefCountPtr(const RefCountPtr<U>& other)
    {
        m_rawPtr = static_cast<T*>(other.Get());
        if (m_rawPtr)
        {
            m_rawPtr->AddRef();
        }
    }

    RefCountPtr(RefCountPtr&& other) noexcept
    {
        m_rawPtr       = other.m_rawPtr;
        other.m_rawPtr = nullptr;
    }

    template <class U> explicit RefCountPtr(RefCountPtr<U>&& other)
    {
        m_rawPtr       = static_cast<T*>(other.Get());
        other.m_rawPtr = nullptr;
    }

    ~RefCountPtr()
    {
        if (m_rawPtr)
        {
            m_rawPtr->Release();
        }
    }

    RefCountPtr& operator=(T* ptr)
    {
        if (m_rawPtr != ptr)
        {
            T* oldPtr = m_rawPtr;
            m_rawPtr  = ptr;
            if (m_rawPtr)
            {
                m_rawPtr->AddRef();
            }
            if (oldPtr)
            {
                oldPtr->Release();
            }
        }

        return *this;
    }

    RefCountPtr& operator=(const RefCountPtr& other) // NOLINT(bugprone-unhandled-self-assignment)
    {
        if (m_rawPtr != other.m_rawPtr)
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
            T* oldPtr      = m_rawPtr;
            m_rawPtr       = other.m_rawPtr;
            other.m_rawPtr = nullptr;
            if (oldPtr)
            {
                oldPtr->Release();
            }
        }
        return *this;
    }

    template <class U> RefCountPtr& operator=(RefCountPtr<U>&& other) noexcept
    {
        T* oldPtr      = m_rawPtr;
        m_rawPtr       = other.m_rawPtr;
        other.m_rawPtr = nullptr;
        if (oldPtr)
        {
            oldPtr->Release();
        }
        return *this;
    }

    T* operator->() const
    {
        return m_rawPtr;
    }

    operator T*() const
    {
        return m_rawPtr;
    }

    T** operator&() // NOLINT(google-runtime-operator)
    {
        return &m_rawPtr;
    }

    uint32_t GetRefCount()
    {
        uint32_t count = 0;
        if (m_rawPtr)
        {
            count = m_rawPtr->GetRefCount();
            assert(count > 0);
        }
        return count;
    }

    void Swap(RefCountPtr&& other) noexcept
    {
        T* tmp         = m_rawPtr;
        m_rawPtr       = other.m_rawPtr;
        other.m_rawPtr = tmp;
    }

    void Swap(RefCountPtr& other) noexcept
    {
        T* tmp         = m_rawPtr;
        m_rawPtr       = other.m_rawPtr;
        other.m_rawPtr = tmp;
    }

private:
    T* m_rawPtr;
};

template <class T, class... Args> RefCountPtr<T> MakeRefCountPtr(Args&&... args_)
{
    return RefCountPtr<T>(new T(std::forward<Args>(args_)...));
}
} // namespace zen
