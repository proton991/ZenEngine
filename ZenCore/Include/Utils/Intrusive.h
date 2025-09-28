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

    explicit IntrusivePtr(T* ptr)
    {
        m_ptr = ptr;
    }

    T& operator*()
    {
        return *m_ptr;
    }

    const T& operator*() const
    {
        return *m_ptr;
    }

    T* operator->()
    {
        return m_ptr;
    }

    const T* operator->() const
    {
        return m_ptr;
    }

    explicit operator bool() const
    {
        return m_ptr != nullptr;
    }

    bool operator==(const IntrusivePtr& other) const
    {
        return m_ptr == other.m_ptr;
    }

    bool operator!=(const IntrusivePtr& other) const
    {
        return m_ptr != other.m_ptr;
    }

    T* Get()
    {
        return m_ptr;
    }

    const T* Get() const
    {
        return m_ptr;
    }

    void Reset()
    {
        using ReferenceBase =
            IntrusivePtrEnabled<typename T::EnabledBase, typename T::EnabledDeleter,
                                typename T::EnabledReferenceCounterType>;
        // Static up-cast here to avoid potential issues with multiple intrusive inheritance.
        // Also makes sure that the pointer type actually inherits from this type.
        if (m_ptr)
            static_cast<ReferenceBase*>(m_ptr)->ReleaseReference();
        m_ptr = nullptr;
    }

    template <typename U> IntrusivePtr& operator=(const IntrusivePtr<U>& other)
    {
        static_assert(std::is_base_of<T, U>::value,
                      "Cannot safely assign downcast intrusive pointers.");

        using ReferenceBase =
            IntrusivePtrEnabled<typename T::EnabledBase, typename T::EnabledDeleter,
                                typename T::EnabledReferenceCounterType>;

        Reset();
        m_ptr = static_cast<T*>(other.m_ptr);

        // Static up-cast here to avoid potential issues with multiple intrusive inheritance.
        // Also makes sure that the pointer type actually inherits from this type.
        if (m_ptr)
            static_cast<ReferenceBase*>(m_ptr)->AddReference();
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
            m_ptr = other.m_ptr;
            if (m_ptr)
                static_cast<ReferenceBase*>(m_ptr)->AddReference();
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
        m_ptr       = other.m_ptr;
        other.m_ptr = nullptr;
        return *this;
    }

    IntrusivePtr& operator=(IntrusivePtr&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            m_ptr       = other.m_ptr;
            other.m_ptr = nullptr;
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
        T* ret = m_ptr;
        m_ptr  = nullptr;
        return ret;
    }

    T* Release() &&
    {
        T* ret = m_ptr;
        m_ptr  = nullptr;
        return ret;
    }

private:
    T* m_ptr{nullptr};
};

template <class T, class... Args> IntrusivePtr<T> MakeIntrusive(Args&&... args_)
{
    return IntrusivePtr<T>(new T(std::forward<Args>(args_)...));
}

} // namespace zen