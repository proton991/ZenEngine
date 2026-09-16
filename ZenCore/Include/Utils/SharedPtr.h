#pragma once
#include "Counter.h"
#include <cstddef>
#include <type_traits>
#include <utility>

namespace zen
{
namespace detail
{
template <class RefCounterType> struct SharedPtrControlBlock
{
    virtual ~SharedPtrControlBlock() = default;
    RefCounterType count;
};

template <class T> struct SharedPtrDelete
{
    void operator()(T* object) const noexcept
    {
        delete object;
    }
};

template <class T, class Deleter, class RefCounterType> struct SharedPtrOwnedBlock final :
    SharedPtrControlBlock<RefCounterType>
{
    SharedPtrOwnedBlock(T* object, Deleter deleter) : object(object), deleter(std::move(deleter)) {}

    ~SharedPtrOwnedBlock() override
    {
        deleter(object);
    }

    T* object;
    Deleter deleter;
};

// Retain ownership until control-block construction has succeeded.
template <class T, class Deleter> struct SharedPtrPendingOwner
{
    ~SharedPtrPendingOwner()
    {
        if (pending)
        {
            deleter(object);
        }
    }

    T* object;
    Deleter& deleter;
    bool pending{true};
};
} // namespace detail

// MultiThreadCounter protects ownership across separate pointer instances.
// Concurrent mutation of the same SharedPtr still requires external synchronization.
template <class T, class RefCounterType = SingleThreadCounter> class SharedPtr
{
public:
    using ElementType = T;

    SharedPtr() noexcept = default;
    SharedPtr(std::nullptr_t) noexcept {}

    template <class U>
        requires std::is_convertible_v<U*, T*>
    explicit SharedPtr(U* object)
    {
        if (object != nullptr)
        {
            Acquire(object, detail::SharedPtrDelete<U>{});
        }
    }

    template <class U, class Deleter>
        requires std::is_convertible_v<U*, T*>
    SharedPtr(U* object, Deleter deleter)
    {
        Acquire(object, std::move(deleter));
    }

    // Aliases retain the original object and deleter, including null aliases.
    template <class U> SharedPtr(const SharedPtr<U, RefCounterType>& owner, T* object) noexcept :
        m_pObj(object), m_control(owner.m_control)
    {
        AddReference();
    }

    template <class U>
        requires std::is_convertible_v<U*, T*>
    SharedPtr(const SharedPtr<U, RefCounterType>& other) noexcept :
        m_pObj(other.m_pObj), m_control(other.m_control)
    {
        AddReference();
    }

    SharedPtr(const SharedPtr& other) noexcept : m_pObj(other.m_pObj), m_control(other.m_control)
    {
        AddReference();
    }

    template <class U>
        requires std::is_convertible_v<U*, T*>
    SharedPtr(SharedPtr<U, RefCounterType>&& other) noexcept :
        m_pObj(std::exchange(other.m_pObj, nullptr)),
        m_control(std::exchange(other.m_control, nullptr))
    {}

    SharedPtr(SharedPtr&& other) noexcept :
        m_pObj(std::exchange(other.m_pObj, nullptr)),
        m_control(std::exchange(other.m_control, nullptr))
    {}

    SharedPtr& operator=(const SharedPtr& other) noexcept
    {
        SharedPtr copy(other);
        Swap(copy);
        return *this;
    }

    SharedPtr& operator=(SharedPtr&& other) noexcept
    {
        SharedPtr moved(std::move(other));
        Swap(moved);
        return *this;
    }

    ~SharedPtr() noexcept
    {
        Release();
    }

    T* Get() const noexcept
    {
        return m_pObj;
    }

    T* operator->() const noexcept
    {
        return m_pObj;
    }

    T& operator*() const noexcept
    {
        return *m_pObj;
    }

    operator bool() const noexcept
    {
        return m_pObj != nullptr;
    }

    bool Unique() const noexcept
    {
        return UseCount() == 1;
    }

    uint32_t UseCount() const noexcept
    {
        return m_control != nullptr ? m_control->count.GetValue() : 0;
    }

    void Reset() noexcept
    {
        SharedPtr empty;
        Swap(empty);
    }

    void Reset(std::nullptr_t) noexcept
    {
        Reset();
    }

    template <class U>
        requires std::is_convertible_v<U*, T*>
    void Reset(U* object)
    {
        SharedPtr replacement(object);
        Swap(replacement);
    }

    template <class U, class Deleter>
        requires std::is_convertible_v<U*, T*>
    void Reset(U* object, Deleter deleter)
    {
        SharedPtr replacement(object, std::move(deleter));
        Swap(replacement);
    }

    void Swap(SharedPtr& other) noexcept
    {
        std::swap(m_pObj, other.m_pObj);
        std::swap(m_control, other.m_control);
    }

private:
    template <class, class> friend class SharedPtr;

    template <class U, class Deleter> void Acquire(U* object, Deleter deleter)
    {
        static_assert(std::is_nothrow_move_constructible_v<Deleter>);
        detail::SharedPtrPendingOwner<U, Deleter> owner{object, deleter};
        m_control =
            new detail::SharedPtrOwnedBlock<U, Deleter, RefCounterType>(object, std::move(deleter));
        m_pObj        = object;
        owner.pending = false;
    }

    void AddReference() noexcept
    {
        if (m_control != nullptr)
        {
            m_control->count.Add();
        }
    }

    void Release() noexcept
    {
        if (m_control != nullptr && m_control->count.Release())
        {
            delete m_control;
        }
    }

    T* m_pObj{nullptr};
    detail::SharedPtrControlBlock<RefCounterType>* m_control{nullptr};
};

template <class T, class U, class LeftCounter, class RightCounter>
bool operator==(const SharedPtr<T, LeftCounter>& left,
                const SharedPtr<U, RightCounter>& right) noexcept
{
    return left.Get() == right.Get();
}

template <class T, class Counter>
bool operator==(const SharedPtr<T, Counter>& pointer, std::nullptr_t) noexcept
{
    return pointer.Get() == nullptr;
}

template <class T, class U, class LeftCounter, class RightCounter>
bool operator<(const SharedPtr<T, LeftCounter>& left,
               const SharedPtr<U, RightCounter>& right) noexcept
{
    return left.Get() < right.Get();
}

template <class T, class U, class LeftCounter, class RightCounter>
bool operator<=(const SharedPtr<T, LeftCounter>& left,
                const SharedPtr<U, RightCounter>& right) noexcept
{
    return left.Get() <= right.Get();
}

template <class T, class U, class LeftCounter, class RightCounter>
bool operator>(const SharedPtr<T, LeftCounter>& left,
               const SharedPtr<U, RightCounter>& right) noexcept
{
    return right < left;
}

template <class T, class U, class LeftCounter, class RightCounter>
bool operator>=(const SharedPtr<T, LeftCounter>& left,
                const SharedPtr<U, RightCounter>& right) noexcept
{
    return right <= left;
}

template <class T, class U, class Counter>
SharedPtr<T, Counter> static_pointer_cast(const SharedPtr<U, Counter>& pointer) noexcept
{
    return SharedPtr<T, Counter>(pointer, static_cast<T*>(pointer.Get()));
}

template <class T, class U, class Counter>
SharedPtr<T, Counter> dynamic_pointer_cast(const SharedPtr<U, Counter>& pointer) noexcept
{
    SharedPtr<T, Counter> result;
    T* object = dynamic_cast<T*>(pointer.Get());
    if (object != nullptr)
    {
        result = SharedPtr<T, Counter>(pointer, object);
    }
    return result;
}

template <class T, class RefCounterType = SingleThreadCounter, class... Args>
SharedPtr<T, RefCounterType> MakeShared(Args&&... args)
{
    return SharedPtr<T, RefCounterType>(new T(std::forward<Args>(args)...));
}

} // namespace zen