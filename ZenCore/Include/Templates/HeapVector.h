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

    explicit HeapVector(size_type count)
    {
        resize(count);
    }

    // explicit HeapVector(size_type reserveCount)
    // {
    //     reserve(reserveCount);
    // }

    HeapVector(std::initializer_list<T> init)
    {
        reserve(init.size());
        for (const T& v : init)
            emplace_back(v);
    }

    HeapVector(const T* pSrcData, size_type count)
    {
        if (count == 0)
            return;

        reserve(count);

        if constexpr (std::is_trivially_copy_constructible_v<T>)
        {
            std::memcpy(m_pData, pSrcData, sizeof(T) * count);
            m_size = count;
        }
        else
        {
            for (size_type i = 0; i < count; ++i)
            {
                new (&m_pData[i]) T(pSrcData[i]);
            }
            m_size = count;
        }
    }

    ~HeapVector()
    {
        destroy_range(0, m_size);
        if (m_pData)
        {
            ZEN_MEM_FREE(m_pData);
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
            if (m_pData)
                ZEN_MEM_FREE(m_pData);
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

    iterator erase(const_iterator pos)
    {
        ASSERT(pos >= begin() && pos < end());

        const size_type index = static_cast<size_type>(pos - begin());

        // Destroy the element at pos
        m_pData[index].~T();

        // Move elements left
        if constexpr (std::is_trivially_move_assignable_v<T>)
        {
            std::memmove(m_pData + index, m_pData + index + 1, sizeof(T) * (m_size - index - 1));
        }
        else
        {
            for (size_type i = index; i < m_size - 1; ++i)
            {
                new (&m_pData[i]) T(std::move(m_pData[i + 1]));
                m_pData[i + 1].~T();
            }
        }

        --m_size;
        return m_pData + index;
    }

    iterator erase(const_iterator first, const_iterator last)
    {
        ASSERT(first >= begin() && first <= end());
        ASSERT(last >= first && last <= end());

        const size_type firstIndex = static_cast<size_type>(first - begin());
        const size_type lastIndex  = static_cast<size_type>(last - begin());
        const size_type count      = lastIndex - firstIndex;

        if (count == 0)
            return m_pData + firstIndex;

        // Destroy erased elements
        destroy_range(firstIndex, lastIndex);

        // Move tail
        if constexpr (std::is_trivially_move_assignable_v<T>)
        {
            std::memmove(m_pData + firstIndex, m_pData + lastIndex, sizeof(T) * (m_size - lastIndex));
        }
        else
        {
            for (size_type i = firstIndex; i < m_size - count; ++i)
            {
                new (&m_pData[i]) T(std::move(m_pData[i + count]));
                m_pData[i + count].~T();
            }
        }

        m_size -= count;
        return m_pData + firstIndex;
    }

    void remove(size_type index)
    {
        ASSERT(index < m_size);

        // Destroy target element
        m_pData[index].~T();

        // Move elements left
        if constexpr (std::is_trivially_move_assignable_v<T>)
        {
            std::memmove(m_pData + index, m_pData + index + 1, sizeof(T) * (m_size - index - 1));
        }
        else
        {
            for (size_type i = index; i < m_size - 1; ++i)
            {
                new (&m_pData[i]) T(std::move(m_pData[i + 1]));
                m_pData[i + 1].~T();
            }
        }

        --m_size;
    }


    // ----------- element access -----------

    T& operator[](size_type index)
    {
        ASSERT(index < m_size);
        return m_pData[index];
    }

    const T& operator[](size_type index) const
    {
        ASSERT(index < m_size);
        return m_pData[index];
    }

    T& back()
    {
        ASSERT(m_size > 0);
        return m_pData[m_size - 1];
    }

    const T& back() const
    {
        ASSERT(m_size > 0);
        return m_pData[m_size - 1];
    }

    T* data() noexcept
    {
        return m_pData;
    }
    const T* data() const noexcept
    {
        return m_pData;
    }

    // ----------- iteration -----------

    iterator begin() noexcept
    {
        return m_pData;
    }
    iterator end() noexcept
    {
        return m_pData + m_size;
    }

    const_iterator begin() const noexcept
    {
        return m_pData;
    }
    const_iterator end() const noexcept
    {
        return m_pData + m_size;
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
        T* pElem = new (&m_pData[m_size]) T(std::forward<Args>(args)...);
        ++m_size;
        return *pElem;
    }

    void pop_back()
    {
        ASSERT(m_size > 0);
        --m_size;
        m_pData[m_size].~T();
    }

private:
    T* m_pData            = nullptr;
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
            m_pData = static_cast<T*>(ZEN_MEM_REALLOC_ALIGNED(m_pData, newBytes, alignof(T)));
        }
        else
        {
            T* pNewMem = static_cast<T*>(ZEN_MEM_ALLOC_ALIGNED(newBytes, alignof(T)));

            for (size_type i = 0; i < m_size; ++i)
            {
                new (&pNewMem[i]) T(std::move(m_pData[i]));
                m_pData[i].~T();
            }

            if (m_pData)
                ZEN_MEM_FREE(m_pData);

            m_pData = pNewMem;
        }

        m_capacity = newCapacity;
    }

    // void grow(size_type newCapacity)
    // {
    //     const size_t newBytes   = sizeof(T) * newCapacity;
    //     constexpr size_t kAlign = alignof(T);
    //
    //     if constexpr (std::is_trivially_move_constructible_v<T>)
    //     {
    //         if constexpr (kAlign <= ZEN_DEFAULT_ALIGNMENT)
    //         {
    //             m_pData = static_cast<T*>(ZEN_MEM_REALLOC(m_pData, newBytes));
    //         }
    //         else
    //         {
    //             m_pData = static_cast<T*>(ZEN_MEM_REALLOC_ALIGNED(m_pData, newBytes, kAlign));
    //         }
    //     }
    //     else
    //     {
    //         T* newMem = nullptr;
    //
    //         if constexpr (kAlign <= ZEN_DEFAULT_ALIGNMENT)
    //         {
    //             newMem = static_cast<T*>(ZEN_MEM_ALLOC(newBytes));
    //         }
    //         else
    //         {
    //             newMem = static_cast<T*>(ZEN_MEM_ALLOC_ALIGNED(newBytes, kAlign));
    //         }
    //
    //         for (size_type i = 0; i < m_size; ++i)
    //         {
    //             new (&newMem[i]) T(std::move(m_pData[i]));
    //             m_pData[i].~T();
    //         }
    //
    //         if (m_pData)
    //             ZEN_MEM_FREE(m_pData);
    //
    //         m_pData = newMem;
    //     }
    //
    //     m_capacity = newCapacity;
    // }

    void destroy_range(size_type begin, size_type end)
    {
        for (size_type i = begin; i < end; ++i)
            m_pData[i].~T();
    }

    void construct_range(size_type begin, size_type end)
    {
        for (size_type i = begin; i < end; ++i)
            new (&m_pData[i]) T();
    }

    void move_from(HeapVector& other)
    {
        m_pData     = other.m_pData;
        m_size     = other.m_size;
        m_capacity = other.m_capacity;

        other.m_pData     = nullptr;
        other.m_size     = 0;
        other.m_capacity = 0;
    }
};

} // namespace zen
