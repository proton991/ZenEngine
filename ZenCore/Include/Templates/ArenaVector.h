#pragma once
#include <cstring>
#include <utility>
#include "Memory/LinearAllocator.h"

namespace zen
{
template <typename T, typename Allocator> class ArenaVector
{
public:
    using value_type     = T;
    using iterator       = T*;
    using const_iterator = const T*;
    using size_type      = size_t;

    explicit ArenaVector(Allocator* pAllocator) : m_pAllocator(pAllocator)
    {
        ReserveInternal(8); // default small reserve
    }

    ArenaVector(Allocator* pAllocator, size_t reserveCount) : m_pAllocator(pAllocator)
    {
        ReserveInternal(reserveCount);
    }

    ~ArenaVector()
    {
        DestroyElements();
        // Memory is NOT freed here — PoolAllocator manages lifetime!
    }

    ArenaVector(const ArenaVector&)            = delete;
    ArenaVector& operator=(const ArenaVector&) = delete;

    ArenaVector(ArenaVector&& other) noexcept
    {
        MoveFrom(other);
    }

    ArenaVector& operator=(ArenaVector&& other) noexcept
    {
        if (this != &other)
        {
            DestroyElements();
            MoveFrom(other);
        }
        return *this;
    }

    // ----------- Capacity -----------
    size_t size() const
    {
        return m_size;
    }
    size_t capacity() const
    {
        return m_capacity;
    }
    bool empty() const
    {
        return m_size == 0;
    }

    // ----------- Element Access -----------
    T& operator[](size_t i)
    {
        assert(i < m_size);
        return m_pData[i];
    }
    const T& operator[](size_t i) const
    {
        assert(i < m_size);
        return m_pData[i];
    }

    T& back()
    {
        assert(m_size > 0);
        return m_pData[m_size - 1];
    }
    const T& back() const
    {
        assert(m_size > 0);
        return m_pData[m_size - 1];
    }

    // ----------- Iteration -----------
    iterator begin()
    {
        return m_pData;
    }
    const_iterator begin() const
    {
        return m_pData;
    }
    iterator end()
    {
        return m_pData + m_size;
    }
    const_iterator end() const
    {
        return m_pData + m_size;
    }

    // ----------- Modifiers -----------

    void clear()
    {
        DestroyElements();
        m_size = 0;
    }

    void reserve(size_t newCap)
    {
        if (newCap > m_capacity)
            ReserveInternal(newCap);
    }

    void push_back(const T& value)
    {
        EnsureCapacityForOne();
        new (&m_pData[m_size]) T(value);
        m_size++;
    }

    void push_back(T&& value)
    {
        EnsureCapacityForOne();
        new (&m_pData[m_size]) T(std::move(value));
        m_size++;
    }

    template <typename... Args> T& emplace_back(Args&&... args)
    {
        EnsureCapacityForOne();
        T* pElem = new (&m_pData[m_size]) T(std::forward<Args>(args)...);
        m_size++;
        return *pElem;
    }

private:
    Allocator* m_pAllocator = nullptr;

    T* m_pData         = nullptr;
    size_t m_size     = 0;
    size_t m_capacity = 0;

private:
    // Allocate capacity from arena (PoolAllocator will grow pages automatically)
    void ReserveInternal(size_t newCap)
    {
        T* pNewMem = static_cast<T*>(m_pAllocator->Alloc(sizeof(T) * newCap, alignof(T)));
        assert(pNewMem && "ArenaVector: allocator returned null (out of memory?)");

        // move old elements into new arena memory
        if (m_pData)
        {
            for (size_t i = 0; i < m_size; i++)
            {
                new (&pNewMem[i]) T(std::move(m_pData[i]));
                m_pData[i].~T();
            }
        }

        m_pData     = pNewMem;
        m_capacity = newCap;
    }

    void EnsureCapacityForOne()
    {
        if (m_size == m_capacity)
        {
            size_t newCap = (m_capacity == 0) ? 4 : m_capacity * 2;
            ReserveInternal(newCap);
        }
    }

    void DestroyElements()
    {
        for (size_t i = 0; i < m_size; i++)
            m_pData[i].~T();
    }

    void MoveFrom(ArenaVector& other)
    {
        m_pAllocator = other.m_pAllocator;
        m_pData      = other.m_pData;
        m_size      = other.m_size;
        m_capacity  = other.m_capacity;

        other.m_pData     = nullptr;
        other.m_size     = 0;
        other.m_capacity = 0;
    }
};
} // namespace zen