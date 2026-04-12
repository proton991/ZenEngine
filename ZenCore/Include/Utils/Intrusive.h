#pragma once
#include "Counter.h"
#include <memory>
#include "ObjectBase.h"

namespace zen
{
template <class T> class IntrusivePtr;

template <class T,
          class Deleter        = std::default_delete<T>,
          class RefCounterType = SingleThreadCounter>
class IntrusivePtrEnabled
{
public:
    ZEN_NO_COPY(IntrusivePtrEnabled)
    IntrusivePtrEnabled() = default;

    using IntrusivePtrType            = IntrusivePtr<T>;
    using EnabledBase                 = T;
    using EnabledDeleter              = Deleter;
    using EnabledReferenceCounterType = RefCounterType;


    void ReleaseReference()
    {
        if (m_counter.Release())
        {
            Deleter()(static_cast<T*>(this));
        }
    }

    void AddReference()
    {
        m_counter.Add();
    }


protected:
    IntrusivePtr<T> ReferenceFromThis()
    {
        AddReference();
        return IntrusivePtr<T>(static_cast<T*>(this));
    }

private:
    RefCounterType m_counter;
};

template <class T> class IntrusivePtr
{
public:
    template <class U> friend class IntrusivePtr;

    IntrusivePtr() = default;

    explicit IntrusivePtr(T* pPtr)
    {
        this->m_pPtr = pPtr;
    }

    T& operator*()
    {
        return *m_pPtr;
    }

    const T& operator*() const
    {
        return *m_pPtr;
    }

    T* operator->()
    {
        return m_pPtr;
    }

    const T* operator->() const
    {
        return m_pPtr;
    }

    explicit operator bool() const
    {
        return m_pPtr != nullptr;
    }

    bool operator==(const IntrusivePtr& other) const
    {
        return m_pPtr == other.m_pPtr;
    }

    bool operator!=(const IntrusivePtr& other) const
    {
        return m_pPtr != other.m_pPtr;
    }

    T* Get()
    {
        return m_pPtr;
    }

    const T* Get() const
    {
        return m_pPtr;
    }

    void Reset()
    {
        using ReferenceBase =
            IntrusivePtrEnabled<typename T::EnabledBase, typename T::EnabledDeleter,
                                typename T::EnabledReferenceCounterType>;
        // Static up-cast here to avoid potential issues with multiple intrusive inheritance.
        // Also makes sure that the pointer type actually inherits from this type.
        if (m_pPtr)
            static_cast<ReferenceBase*>(m_pPtr)->ReleaseReference();
        m_pPtr = nullptr;
    }

    template <typename U> IntrusivePtr& operator=(const IntrusivePtr<U>& other)
    {
        static_assert(std::is_base_of<T, U>::value,
                      "Cannot safely assign downcast intrusive pointers.");

        using ReferenceBase =
            IntrusivePtrEnabled<typename T::EnabledBase, typename T::EnabledDeleter,
                                typename T::EnabledReferenceCounterType>;

        Reset();
        m_pPtr = static_cast<T*>(other.m_pPtr);

        // Static up-cast here to avoid potential issues with multiple intrusive inheritance.
        // Also makes sure that the pointer type actually inherits from this type.
        if (m_pPtr)
            static_cast<ReferenceBase*>(m_pPtr)->AddReference();
        return *this;
    }

    IntrusivePtr& operator=(const IntrusivePtr& other)
    {
        using ReferenceBase =
            IntrusivePtrEnabled<typename T::EnabledBase, typename T::EnabledDeleter,
                                typename T::EnabledReferenceCounterType>;

        if (this != &other)
        {
            Reset();
            m_pPtr = other.m_pPtr;
            if (m_pPtr)
                static_cast<ReferenceBase*>(m_pPtr)->AddReference();
        }
        return *this;
    }

    template <typename U> IntrusivePtr(const IntrusivePtr<U>& other)
    {
        *this = other;
    }

    IntrusivePtr(const IntrusivePtr& other)
    {
        *this = other;
    }

    ~IntrusivePtr()
    {
        Reset();
    }

    template <typename U> IntrusivePtr& operator=(IntrusivePtr<U>&& other) noexcept
    {
        Reset();
        m_pPtr       = other.m_pPtr;
        other.m_pPtr = nullptr;
        return *this;
    }

    IntrusivePtr& operator=(IntrusivePtr&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            m_pPtr       = other.m_pPtr;
            other.m_pPtr = nullptr;
        }
        return *this;
    }

    template <typename U> IntrusivePtr(IntrusivePtr<U>&& other) noexcept
    {
        *this = std::move(other);
    }

    template <typename U> IntrusivePtr(IntrusivePtr&& other) noexcept
    {
        *this = std::move(other);
    }

    T* Release() &
    {
        T* pRet = m_pPtr;
        m_pPtr  = nullptr;
        return pRet;
    }

    T* Release() &&
    {
        T* pRet = m_pPtr;
        m_pPtr  = nullptr;
        return pRet;
    }

private:
    T* m_pPtr{nullptr};
};

template <class T, class... Args> IntrusivePtr<T> MakeIntrusive(Args&&... args_)
{
    return IntrusivePtr<T>(new T(std::forward<Args>(args_)...));
}

} // namespace zen
