#pragma once
#include "Utils/Errors.h"
#include "Memory/Memory.h"

namespace zen
{
template <typename T> class HeapVector
{
public:
    using value_type     = T;
    using size_type      = size_t;
    using iterator       = T*;
    using const_iterator = const T*;

    // ----------- ctor / dtor -----------

    HeapVector() = default;

    explicit HeapVector(size_type reserveCount)
    {
        reserve(reserveCount);
    }

    HeapVector(std::initializer_list<T> init)
    {
        reserve(init.size());
        for (const T& v : init)
            emplace_back(v);
    }

    ~HeapVector()
    {
        destroy_range(0, m_size);
        if (m_data)
        {
            ZEN_MEM_FREE(m_data);
        }
    }

    HeapVector(const HeapVector&)            = delete;
    HeapVector& operator=(const HeapVector&) = delete;

    HeapVector(HeapVector&& other) noexcept
    {
        move_from(other);
    }

    HeapVector& operator=(HeapVector&& other) noexcept
    {
        if (this != &other)
        {
            clear();
            if (m_data)
                ZEN_MEM_FREE(m_data);
            move_from(other);
        }
        return *this;
    }

    // ----------- capacity -----------

    size_type size() const noexcept
    {
        return m_size;
    }
    size_type capacity() const noexcept
    {
        return m_capacity;
    }
    bool empty() const noexcept
    {
        return m_size == 0;
    }

    void reserve(size_type newCapacity)
    {
        if (newCapacity > m_capacity)
            grow(newCapacity);
    }

    void resize(size_type newSize)
    {
        if (newSize < m_size)
        {
            destroy_range(newSize, m_size);
        }
        else if (newSize > m_size)
        {
            reserve(newSize);
            construct_range(m_size, newSize);
        }
        m_size = newSize;
    }

    void clear()
    {
        destroy_range(0, m_size);
        m_size = 0;
    }

    // ----------- element access -----------

    T& operator[](size_type index)
    {
        ASSERT(index < m_size);
        return m_data[index];
    }

    const T& operator[](size_type index) const
    {
        ASSERT(index < m_size);
        return m_data[index];
    }

    T& back()
    {
        ASSERT(m_size > 0);
        return m_data[m_size - 1];
    }

    const T& back() const
    {
        ASSERT(m_size > 0);
        return m_data[m_size - 1];
    }

    T* data() noexcept
    {
        return m_data;
    }
    const T* data() const noexcept
    {
        return m_data;
    }

    // ----------- iteration -----------

    iterator begin() noexcept
    {
        return m_data;
    }
    iterator end() noexcept
    {
        return m_data + m_size;
    }

    const_iterator begin() const noexcept
    {
        return m_data;
    }
    const_iterator end() const noexcept
    {
        return m_data + m_size;
    }

    // ----------- modifiers -----------

    void push_back(const T& value)
    {
        emplace_back(value);
    }

    void push_back(T&& value)
    {
        emplace_back(std::move(value));
    }

    template <typename... Args> T& emplace_back(Args&&... args)
    {
        ensure_capacity_for_one();
        T* elem = new (&m_data[m_size]) T(std::forward<Args>(args)...);
        ++m_size;
        return *elem;
    }

    void pop_back()
    {
        ASSERT(m_size > 0);
        --m_size;
        m_data[m_size].~T();
    }

private:
    T* m_data            = nullptr;
    size_type m_size     = 0;
    size_type m_capacity = 0;

private:
    void ensure_capacity_for_one()
    {
        if (m_size == m_capacity)
        {
            size_type newCap = (m_capacity == 0) ? 4 : m_capacity * 2;
            grow(newCap);
        }
    }

    void grow(size_type newCapacity)
    {
        const size_t newBytes = sizeof(T) * newCapacity;

        if constexpr (std::is_trivially_move_constructible_v<T>)
        {
            m_data = static_cast<T*>(ZEN_MEM_REALLOC_ALIGNED(m_data, newBytes, alignof(T)));
        }
        else
        {
            T* newMem = static_cast<T*>(ZEN_MEM_ALLOC_ALIGNED(newBytes, alignof(T)));

            for (size_type i = 0; i < m_size; ++i)
            {
                new (&newMem[i]) T(std::move(m_data[i]));
                m_data[i].~T();
            }

            if (m_data)
                ZEN_MEM_FREE(m_data);

            m_data = newMem;
        }

        m_capacity = newCapacity;
    }

    void destroy_range(size_type begin, size_type end)
    {
        for (size_type i = begin; i < end; ++i)
            m_data[i].~T();
    }

    void construct_range(size_type begin, size_type end)
    {
        for (size_type i = begin; i < end; ++i)
            new (&m_data[i]) T();
    }

    void move_from(HeapVector& other)
    {
        m_data     = other.m_data;
        m_size     = other.m_size;
        m_capacity = other.m_capacity;

        other.m_data     = nullptr;
        other.m_size     = 0;
        other.m_capacity = 0;
    }
};

} // namespace zen
