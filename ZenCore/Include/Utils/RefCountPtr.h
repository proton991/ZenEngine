#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace zen
{
// Atomic ownership protects separate handles, not concurrent access to the object
// or mutation of the same handle. New objects begin without an owning reference.
class RefCounted
{
public:
    RefCounted()                             = default;
    RefCounted(const RefCounted&)            = delete;
    RefCounted& operator=(const RefCounted&) = delete;

    virtual ~RefCounted()
    {
        assert(GetRefCount() == 0);
    }

    uint32_t AddRef() const noexcept
    {
        return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    uint32_t Release() const noexcept
    {
        // The final owner observes writes published by earlier releasing owners.
        const uint32_t previous = m_refCount.fetch_sub(1, std::memory_order_acq_rel);
        assert(previous != 0);
        if (previous == 1)
        {
            const_cast<RefCounted*>(this)->OnFinalRelease();
        }
        return previous - 1;
    }

    uint32_t GetRefCount() const noexcept
    {
        return m_refCount.load(std::memory_order_relaxed);
    }

protected:
    // Override when an object needs a specific allocator or destruction thread.
    virtual void OnFinalRelease()
    {
        delete this;
    }

private:
    mutable std::atomic<uint32_t> m_refCount{0};
};

struct DefaultRefCountPolicy
{
    template <class T> static void AddRef(T* object)
    {
        object->AddRef();
    }

    template <class T> static void Release(T* object)
    {
        object->Release();
    }
};

// A policy adapts an existing intrusive counter without adding another counter.
// Conversions preserve the policy and only allow implicit pointer conversions.
template <class T, class RefCountPolicy = DefaultRefCountPolicy> class RefCountPtr
{
public:
    RefCountPtr() noexcept = default;
    RefCountPtr(std::nullptr_t) noexcept {}

    // addRef=false adopts one reference already held by the caller.
    explicit RefCountPtr(T* object, bool addRef = true) noexcept : m_pRawPtr(object)
    {
        if (m_pRawPtr != nullptr && addRef)
        {
            RefCountPolicy::AddRef(m_pRawPtr);
        }
    }

    RefCountPtr(const RefCountPtr& other) noexcept : RefCountPtr(other.Get()) {}

    template <class U>
        requires std::is_convertible_v<U*, T*>
    RefCountPtr(const RefCountPtr<U, RefCountPolicy>& other) noexcept : RefCountPtr(other.Get())
    {}

    RefCountPtr(RefCountPtr&& other) noexcept : m_pRawPtr(other.Detach()) {}

    template <class U>
        requires std::is_convertible_v<U*, T*>
    RefCountPtr(RefCountPtr<U, RefCountPolicy>&& other) noexcept : m_pRawPtr(other.Detach())
    {}

    ~RefCountPtr()
    {
        if (m_pRawPtr != nullptr)
        {
            RefCountPolicy::Release(m_pRawPtr);
        }
    }

    RefCountPtr& operator=(T* object) noexcept
    {
        Reset(object);
        return *this;
    }

    RefCountPtr& operator=(const RefCountPtr& other) noexcept
    {
        RefCountPtr copy(other);
        Swap(copy);
        return *this;
    }

    template <class U>
        requires std::is_convertible_v<U*, T*>
    RefCountPtr& operator=(const RefCountPtr<U, RefCountPolicy>& other) noexcept
    {
        RefCountPtr copy(other);
        Swap(copy);
        return *this;
    }

    RefCountPtr& operator=(RefCountPtr&& other) noexcept
    {
        RefCountPtr moved(std::move(other));
        Swap(moved);
        return *this;
    }

    template <class U>
        requires std::is_convertible_v<U*, T*>
    RefCountPtr& operator=(RefCountPtr<U, RefCountPolicy>&& other) noexcept
    {
        RefCountPtr moved(std::move(other));
        Swap(moved);
        return *this;
    }

    T* Get() const noexcept
    {
        return m_pRawPtr;
    }

    T* operator->() const noexcept
    {
        return m_pRawPtr;
    }

    T& operator*() const noexcept
    {
        return *m_pRawPtr;
    }

    explicit operator bool() const noexcept
    {
        return m_pRawPtr != nullptr;
    }

    uint32_t GetRefCount() const noexcept
    {
        return m_pRawPtr != nullptr ? m_pRawPtr->GetRefCount() : 0;
    }

    void Reset(T* object = nullptr) noexcept
    {
        RefCountPtr replacement(object);
        Swap(replacement);
    }

    // Transfer an existing reference without incrementing or releasing it.
    static RefCountPtr Adopt(T* object) noexcept
    {
        return RefCountPtr(object, false);
    }

    T* Detach() noexcept
    {
        return std::exchange(m_pRawPtr, nullptr);
    }

    void Swap(RefCountPtr& other) noexcept
    {
        std::swap(m_pRawPtr, other.m_pRawPtr);
    }

private:
    T* m_pRawPtr{nullptr};
};

template <class T, class U, class Policy>
bool operator==(const RefCountPtr<T, Policy>& left, const RefCountPtr<U, Policy>& right) noexcept
{
    return left.Get() == right.Get();
}

template <class T, class Policy>
bool operator==(const RefCountPtr<T, Policy>& pointer, std::nullptr_t) noexcept
{
    return pointer.Get() == nullptr;
}

template <class T, class... Args> RefCountPtr<T> MakeRefCountPtr(Args&&... args)
{
    return RefCountPtr<T>(new T(std::forward<Args>(args)...));
}
} // namespace zen
