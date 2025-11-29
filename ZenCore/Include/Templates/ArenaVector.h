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

    explicit ArenaVector(Allocator* allocator) : m_allocator(allocator)
    {
        ReserveInternal(8); // default small reserve
    }

    ArenaVector(Allocator* allocator, size_t reserveCount) : m_allocator(allocator)
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
        return m_data[i];
    }
    const T& operator[](size_t i) const
    {
        assert(i < m_size);
        return m_data[i];
    }

    T& back()
    {
        assert(m_size > 0);
        return m_data[m_size - 1];
    }
    const T& back() const
    {
        assert(m_size > 0);
        return m_data[m_size - 1];
    }

    // ----------- Iteration -----------
    iterator begin()
    {
        return m_data;
    }
    const_iterator begin() const
    {
        return m_data;
    }
    iterator end()
    {
        return m_data + m_size;
    }
    const_iterator end() const
    {
        return m_data + m_size;
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
        new (&m_data[m_size]) T(value);
        m_size++;
    }

    void push_back(T&& value)
    {
        EnsureCapacityForOne();
        new (&m_data[m_size]) T(std::move(value));
        m_size++;
    }

    template <typename... Args> T& emplace_back(Args&&... args)
    {
        EnsureCapacityForOne();
        T* elem = new (&m_data[m_size]) T(std::forward<Args>(args)...);
        m_size++;
        return *elem;
    }

private:
    Allocator* m_allocator = nullptr;

    T* m_data         = nullptr;
    size_t m_size     = 0;
    size_t m_capacity = 0;

private:
    // Allocate capacity from arena (PoolAllocator will grow pages automatically)
    void ReserveInternal(size_t newCap)
    {
        T* newMem = static_cast<T*>(m_allocator->Alloc(sizeof(T) * newCap, alignof(T)));
        assert(newMem && "ArenaVector: allocator returned null (out of memory?)");

        // move old elements into new arena memory
        if (m_data)
        {
            for (size_t i = 0; i < m_size; i++)
            {
                new (&newMem[i]) T(std::move(m_data[i]));
                m_data[i].~T();
            }
        }

        m_data     = newMem;
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
            m_data[i].~T();
    }

    void MoveFrom(ArenaVector& other)
    {
        m_allocator = other.m_allocator;
        m_data      = other.m_data;
        m_size      = other.m_size;
        m_capacity  = other.m_capacity;

        other.m_data     = nullptr;
        other.m_size     = 0;
        other.m_capacity = 0;
    }
};
} // namespace zen