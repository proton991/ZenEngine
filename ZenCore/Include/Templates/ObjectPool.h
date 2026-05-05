#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>

#include "Memory/Memory.h"
#include "Templates/HeapVector.h"

namespace zen
{
namespace detail
{
template <typename Policy, typename T, typename = void> struct HasObjectPoolPolicy
    : std::false_type
{};

template <typename Policy, typename T>
struct HasObjectPoolPolicy<Policy,
                           T,
                           std::void_t<decltype(Policy::Create()),
                                       decltype(Policy::Reset(std::declval<T*>())),
                                       decltype(Policy::Destroy(std::declval<T*>()))>>
    : std::integral_constant<bool,
                             std::is_convertible_v<decltype(Policy::Create()), T*> &&
                                 std::is_same_v<decltype(Policy::Reset(std::declval<T*>())), void> &&
                                 std::is_same_v<decltype(Policy::Destroy(std::declval<T*>())), void>>
{};
} // namespace detail

template <typename T> struct DefaultObjectPoolPolicy
{
    static T* Create()
    {
        return ZEN_NEW() T();
    }

    static void Reset(T* pObject)
    {
        pObject->Reset();
    }

    static void Destroy(T* pObject)
    {
        ZEN_DELETE(pObject);
    }
};

template <typename T, typename Policy = DefaultObjectPoolPolicy<T>> class ObjectPool
{
    static_assert(detail::HasObjectPoolPolicy<Policy, T>::value,
                  "ObjectPool<T, Policy> requires Policy::Create(), Policy::Reset(T*), and "
                  "Policy::Destroy(T*)");

public:
    ObjectPool() = default;

    ~ObjectPool()
    {
        Destroy();
    }

    ObjectPool(const ObjectPool&)            = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    ObjectPool(ObjectPool&&)            = delete;
    ObjectPool& operator=(ObjectPool&&) = delete;

    T* Acquire()
    {
        T* pObject = nullptr;
        if (!m_freeObjects.empty())
        {
            pObject = m_freeObjects.back();
            m_freeObjects.pop_back();
        }
        else
        {
            pObject = Policy::Create();
            m_allObjects.push_back(pObject);
        }

        Policy::Reset(pObject);
        return pObject;
    }

    void Release(T* pObject)
    {
        if (pObject == nullptr)
        {
            return;
        }

        Policy::Reset(pObject);
        m_freeObjects.push_back(pObject);
    }

    template <typename Func> void ForEachObject(Func&& func) const
    {
        for (T* pObject : m_allObjects)
        {
            func(pObject);
        }
    }

    size_t FreeCount() const
    {
        return m_freeObjects.size();
    }

    size_t TotalCount() const
    {
        return m_allObjects.size();
    }

    void Destroy()
    {
        for (T* pObject : m_allObjects)
        {
            Policy::Destroy(pObject);
        }
        m_freeObjects.clear();
        m_allObjects.clear();
    }

private:
    HeapVector<T*> m_freeObjects;
    HeapVector<T*> m_allObjects;
};
} // namespace zen
