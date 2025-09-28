#pragma once
#include "Counter.h"
#if defined(ZEN_MACOS)
#    include <utility>
#endif

namespace zen
{
template <class RefCounterType> class SharedPtrCount
{
public:
    SharedPtrCount() : m_counter(nullptr) {}

    SharedPtrCount(const SharedPtrCount& other) : m_counter(other.m_counter) {}

    void Swap(SharedPtrCount& other) noexcept
    {
        std::swap(m_counter, other.m_counter);
    }

    uint32_t GetValue() const noexcept
    {
        uint32_t count = 0;
        if (m_counter != nullptr)
        {
            count = m_counter->GetValue();
        }
        return count;
    }

    template <class U> void Acquire(U* p) noexcept
    {
        if (p != nullptr)
        {
            if (m_counter == nullptr)
            {
                m_counter = new RefCounterType();
            }
            else
            {
                m_counter->Add();
            }
        }
    }

    template <class U> void Release(U* p) noexcept
    {
        if (m_counter != nullptr)
        {
            //            m_counter->Dec();
            //            if (m_counter->GetValue() == 0)
            //            {
            //                delete p;
            //                delete m_counter;
            //            }
            if (m_counter->Release())
            {
                delete p;
                delete m_counter;
            }
            m_counter = nullptr;
        }
    }

    void Reset()
    {
        m_counter = nullptr;
    }

private:
    RefCounterType* m_counter;
};

template <class RefCounterType> class SharedPtrBase
{
protected:
    SharedPtrBase() : m_count() {}
    SharedPtrBase(const SharedPtrBase& other) : m_count(other.m_count) {}

    SharedPtrCount<RefCounterType> m_count;
};

template <class T, class RefCounterType = SingleThreadCounter> class SharedPtr :
    public SharedPtrBase<RefCounterType>
{
public:
    using ElementType = std::remove_extent_t<T>;

    SharedPtr() noexcept : SharedPtrBase<RefCounterType>(), m_ptr(nullptr) {}

    explicit SharedPtr(T* p) : SharedPtrBase<RefCounterType>()
    {
        Acquire(p);
    }

    /**
     * @brief Used for pointer cast
     */
    template <class U> SharedPtr(const SharedPtr<U>& ptr, T* p) : SharedPtrBase<RefCounterType>(ptr)
    {
        Acquire(p);
    }

    /**
     * @brief Copy constructor using another pointer type
     */
    template <class U> SharedPtr(const SharedPtr<U>& ptr) noexcept :
        SharedPtrBase<RefCounterType>(ptr)
    {
        Acquire(static_cast<typename SharedPtr<T>::ElementType*>(ptr.Get()));
    }

    SharedPtr(const SharedPtr& other) noexcept : SharedPtrBase<RefCounterType>(other)
    {
        Acquire(other.m_ptr);
    }

    SharedPtr(SharedPtr&& other) noexcept
    {
        m_ptr         = std::move(other.m_ptr);
        this->m_count = std::move(other.m_count);
        other.m_ptr   = nullptr;
        other.m_count.Reset();
    }


    SharedPtr& operator=(const SharedPtr& other) noexcept
    {
        Reset();
        m_ptr         = other.m_ptr;
        this->m_count = other.m_count;
        if (m_ptr != nullptr)
        {
            this->m_count.Acquire(other.m_ptr);
        }
        return *this;
    }

    SharedPtr& operator=(SharedPtr&& other) noexcept
    {
        m_ptr         = std::move(other.m_ptr);
        this->m_count = std::move(other.m_count);
        other.m_ptr   = nullptr;
        other.m_count.Reset();
        return *this;
    }

    T* Get() const noexcept
    {
        return m_ptr;
    }

    T* operator->() const noexcept
    {
        return m_ptr;
    }

    T& operator*() const noexcept
    {
        return *m_ptr;
    }

    operator bool() const noexcept
    {
        return this->m_count.GetValue() > 0;
    }

    bool Unique() const noexcept
    {
        return this->m_count.GetValue() == 1;
    }

    uint32_t UseCount() const noexcept
    {
        return this->m_count.GetValue();
    }

    ~SharedPtr() noexcept
    {
        Release();
    }

    void Reset() noexcept
    {
        Release();
    }

    void Reset(T* p)
    {
        Release();
        Acquire(p);
    }

    void Swap(SharedPtr& lhs)
    {
        std::swap(m_ptr, lhs.m_ptr);
        this->m_count.Swap(lhs.m_count);
    }

private:
    void Acquire(T* p)
    {
        this->m_count.Acquire(p);
        m_ptr = p;
    }

    void Release()
    {
        this->m_count.Release(m_ptr);
        m_ptr = nullptr;
    }
    ElementType* m_ptr;
};

// comparison operators
template <class T, class U>
bool operator==(const SharedPtr<T>& l, const SharedPtr<U>& r) noexcept // never throws
{
    return (l.Get() == r.Get());
}
template <class T, class U>
bool operator!=(const SharedPtr<T>& l, const SharedPtr<U>& r) noexcept // never throws
{
    return (l.Get() != r.Get());
}
template <class T, class U>
bool operator<=(const SharedPtr<T>& l, const SharedPtr<U>& r) noexcept // never throws
{
    return (l.Get() <= r.Get());
}
template <class T, class U>
bool operator<(const SharedPtr<T>& l, const SharedPtr<U>& r) noexcept // never throws
{
    return (l.Get() < r.Get());
}
template <class T, class U>
bool operator>=(const SharedPtr<T>& l, const SharedPtr<U>& r) noexcept // never throws
{
    return (l.Get() >= r.Get());
}
template <class T, class U>
bool operator>(const SharedPtr<T>& l, const SharedPtr<U>& r) noexcept // never throws
{
    return (l.Get() > r.Get());
}


// static cast of SharedPtr
template <class T, class U>
SharedPtr<T> static_pointer_cast(const SharedPtr<U>& ptr) // never throws
{
    return SharedPtr<T>(ptr, static_cast<typename SharedPtr<T>::ElementType*>(ptr.Get()));
}

// dynamic cast of SharedPtr
template <class T, class U>
SharedPtr<T> dynamic_pointer_cast(const SharedPtr<U>& ptr) // never throws
{
    T* p = dynamic_cast<typename SharedPtr<T>::ElementType*>(ptr.Get());
    if (nullptr != p)
    {
        return SharedPtr<T>(ptr, p);
    }
    else
    {
        return SharedPtr<T>();
    }
}

template <class T, class... Args> SharedPtr<T> MakeShared(Args&&... args_)
{
    return SharedPtr<T>(new T(std::forward<Args>(args_)...));
}

} // namespace zen