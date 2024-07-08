#pragma once
#include <algorithm>
#include <cassert>
#define UNIQUE_ASSERT(x) assert(x)

namespace zen
{
template <class T> class UniquePtr
{
public:
    /// The type of the managed object, aliased as member type
    typedef T ElementType;

    /// @brief Default constructor
    UniquePtr() noexcept : // never throws
        m_ptr(nullptr)
    {}
    /// @brief Constructor with the provided pointer to manage
    explicit UniquePtr(T* p) noexcept : // never throws
        m_ptr(p)
    {}
    /**
     * @brief Copy constructor to convert from another pointer type
     */
    template <class U> UniquePtr(const UniquePtr<U>& other) noexcept : // never throws
        m_ptr(static_cast<typename UniquePtr<T>::ElementType*>(other.Get()))
    {
        const_cast<UniquePtr<U>&>(other).Release();
    }

    template <class U> UniquePtr(UniquePtr<U>&& other) noexcept : // never throws
        m_ptr(std::move(static_cast<typename UniquePtr<T>::ElementType*>(other.Get())))
    {
        other.Release();
    }

    /// @brief Copy constructor (used by the copy-and-swap idiom)
    UniquePtr(const UniquePtr& other) noexcept : // never throws
        m_ptr(other.m_ptr)
    {
        const_cast<UniquePtr&>(other).m_ptr = nullptr; // const-cast to force ownership transfer!
    }

    UniquePtr(UniquePtr&& other) noexcept : m_ptr(std::move(other.m_ptr))
    {
        other.m_ptr = nullptr;
    }

    /// @brief Assignment operator using the copy-and-swap idiom (copy constructor and swap method)
    UniquePtr& operator=(UniquePtr other) noexcept // never throws
    {
        Swap(other);
        return *this;
    }
    /// @brief the destructor releases its ownership and Destroy the object
    inline ~UniquePtr() noexcept // never throws
    {
        Destroy();
    }
    /// @brief this reset releases its ownership and Destroy the object
    inline void Reset() noexcept // never throws
    {
        Destroy();
    }
    /// @brief this reset Release its ownership and re-acquire another one
    void Reset(T* p) noexcept // never throws
    {
        UNIQUE_ASSERT((nullptr == p) || (m_ptr != p)); // auto-reset not allowed
        Destroy();
        m_ptr = p;
    }

    /// @brief Swap method for the copy-and-swap idiom (copy constructor and swap method)
    void Swap(UniquePtr& lhs) noexcept // never throws
    {
        std::swap(m_ptr, lhs.m_ptr);
    }

    /// @brief Release the ownership of the m_ptr pointer without destroying the object!
    inline void Release() noexcept // never throws
    {
        m_ptr = nullptr;
    }

    // reference counter operations :
    inline operator bool() const noexcept // never throws
    {
        return (nullptr != m_ptr);
    }

    // underlying pointer operations :
    inline T& operator*() const noexcept // never throws
    {
        UNIQUE_ASSERT(nullptr != m_ptr);
        return *m_ptr;
    }
    inline T* operator->() const noexcept // never throws
    {
        UNIQUE_ASSERT(nullptr != m_ptr);
        return m_ptr;
    }
    inline T* Get() const noexcept // never throws
    {
        // no assert, can return nullptr
        return m_ptr;
    }

private:
    /// @brief Release the ownership of the m_ptr pointer and Destroy the object
    inline void Destroy() noexcept // never throws
    {
        delete m_ptr;
        m_ptr = nullptr;
    }

    /// @brief hack: const-cast Release the ownership of the m_ptr pointer without destroying the object!
    inline void Release() const noexcept // never throws
    {
        m_ptr = nullptr;
    }

private:
    T* m_ptr; //!< Native pointer
};


// comparison operators
template <class T, class U>
inline bool operator==(const UniquePtr<T>& l, const UniquePtr<U>& r) noexcept // never throws
{
    return (l.Get() == r.Get());
}
template <class T, class U>
inline bool operator!=(const UniquePtr<T>& l, const UniquePtr<U>& r) noexcept // never throws
{
    return (l.Get() != r.Get());
}
template <class T, class U>
inline bool operator<=(const UniquePtr<T>& l, const UniquePtr<U>& r) noexcept // never throws
{
    return (l.Get() <= r.Get());
}
template <class T, class U>
inline bool operator<(const UniquePtr<T>& l, const UniquePtr<U>& r) noexcept // never throws
{
    return (l.Get() < r.Get());
}
template <class T, class U>
inline bool operator>=(const UniquePtr<T>& l, const UniquePtr<U>& r) noexcept // never throws
{
    return (l.Get() >= r.Get());
}
template <class T, class U>
inline bool operator>(const UniquePtr<T>& l, const UniquePtr<U>& r) noexcept // never throws
{
    return (l.Get() > r.Get());
}

template <class T, class... Args> UniquePtr<T> MakeUnique(Args&&... args_)
{
    return UniquePtr<T>(new T(std::forward<Args>(args_)...));
}
} // namespace zen