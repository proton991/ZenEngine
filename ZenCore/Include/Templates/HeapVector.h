#pragma once
#include <functional>
#include <initializer_list>
#include <type_traits>
#include <utility>
#include "VectorView.h"
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

    HeapVector(size_type count, const T& value)
    {
        reserve(count);
        for (size_type i = 0; i < count; ++i)
        {
            emplace_back(value);
        }
    }

    HeapVector(std::initializer_list<T> init)
    {
        reserve(init.size());

        for (const T& v : init)
        {
            emplace_back(v);
        }
    }

    HeapVector(VectorView<T> view) : HeapVector(view.data(), view.size()) {}

    HeapVector(const T* pSrcData, size_type count)
    {
        if (count > 0)
        {
            reserve(count);
            if constexpr (std::is_trivially_copyable_v<T>)
            {
                std::memcpy(m_pData, pSrcData, sizeof(T) * count);
            }
            else
            {
                for (size_type i = 0; i < count; ++i)
                {
                    new (&m_pData[i]) T(pSrcData[i]);
                }
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

    HeapVector(const HeapVector& other)
    {
        copy_from(other);
    }

    HeapVector& operator=(const HeapVector& other)
    {
        if (this != &other)
        {
            clear();

            if (m_pData)
            {
                ZEN_MEM_FREE(m_pData);
                m_pData = nullptr;
            }

            m_capacity = 0;

            copy_from(other);
        }

        return *this;
    }

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
            {
                ZEN_MEM_FREE(m_pData);
            }

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
        {
            grow(newCapacity);
        }
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

    // Sets the size without initializing new elements. Callers must write every new
    // element before reading it; readers and decoders use this to avoid filling
    // gigabyte payloads twice.
    void resize_for_overwrite(size_type newSize)
    {
        static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>);
        reserve(newSize);
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
        return erase(pos, pos + 1);
    }

    iterator erase(const_iterator first, const_iterator last)
    {
        iterator result{};

        ASSERT(first >= begin() && first <= end());
        ASSERT(last >= first && last <= end());

        if (first == last)
        {
            result = first == end() ? end() : m_pData + (first - begin());
        }
        else
        {
            const size_type firstIndex = static_cast<size_type>(first - begin());
            const size_type lastIndex  = static_cast<size_type>(last - begin());
            const size_type count      = lastIndex - firstIndex;
            // Destroy erased elements
            destroy_range(firstIndex, lastIndex);

            // Move tail
            if constexpr (std::is_trivially_copyable_v<T>)
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
            result  = m_pData + firstIndex;
        }

        return result;
    }

    void remove(size_type index)
    {
        ASSERT(index < m_size);
        erase(m_pData + index);
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

    T& front()
    {
        ASSERT(m_size > 0);
        return m_pData[0];
    }

    const T& front() const
    {
        ASSERT(m_size > 0);
        return m_pData[0];
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
        return m_size == 0 ? m_pData : m_pData + m_size;
    }

    const_iterator begin() const noexcept
    {
        return m_pData;
    }

    const_iterator end() const noexcept
    {
        return m_size == 0 ? m_pData : m_pData + m_size;
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

    void push_back(const HeapVector<T>& values)
    {
        push_back(VectorView<const T>(values.data(), values.size()));
    }

    void push_back(VectorView<T> values)
    {
        push_back(VectorView<const T>(values.data(), values.size()));
    }

    void push_back(VectorView<const T> values)
    {
        if (!values.empty())
        {
            // Snapshot before reserving so self-append and overlapping views remain valid.
            const HeapVector<T> source(values.data(), values.size());
            reserve(grown_capacity(m_size + source.size()));
            if constexpr (std::is_trivially_copyable_v<T>)
            {
                std::memcpy(m_pData + m_size, source.data(), sizeof(T) * source.size());
                m_size += source.size();
            }
            else
            {
                for (const T& value : source)
                {
                    emplace_back(value);
                }
            }
        }
    }

    template <typename... Args> T& emplace_back(Args&&... args)
    {
        ensure_capacity_for_one();
        T* pElem = new (&m_pData[m_size]) T(std::forward<Args>(args)...);
        ++m_size;

        return *pElem;
    }

    template <typename InputIt> iterator insert(const_iterator pos, InputIt first, InputIt last)
    {
        ASSERT(pos >= begin() && pos <= end());

        const size_type index = static_cast<size_type>(pos - begin());
        if constexpr (IsElementPointer<InputIt>)
        {
            // Snapshot a range inside this vector: reserving and shifting the tail would move it.
            if (first != last && !std::less<const T*>()(first, m_pData) && std::less<const T*>()(first, m_pData + m_size))
            {
                const HeapVector<T> source(first, static_cast<size_type>(last - first));
                insert_range(index, source.data(), source.data() + source.size());
            }
            else
            {
                insert_range(index, first, last);
            }
        }
        else
        {
            insert_range(index, first, last);
        }

        return m_pData + index;
    }

    void pop_back()
    {
        ASSERT(m_size > 0);
        --m_size;
        m_pData[m_size].~T();
    }

    void pop_front()
    {
        ASSERT(m_size > 0);
        erase(begin());
    }

private:
    T*        m_pData    = nullptr;
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

    // Raw pointers to T: their ranges can alias this vector and copy as bytes.
    template <typename InputIt> static constexpr bool IsElementPointer =
        std::is_pointer_v<InputIt> && std::is_same_v<std::remove_cv_t<std::remove_pointer_t<InputIt>>, T>;

    // Inserts [first, last), which must not point into this vector, at index.
    template <typename InputIt> void insert_range(size_type index, InputIt first, InputIt last)
    {
        const size_type count = static_cast<size_type>(last - first);
        if (count != 0)
        {
            const size_type oldSize = m_size;
            reserve(grown_capacity(oldSize + count));

            const size_type tail = oldSize - index;

            // Shift the tail [index, oldSize) right by 'count' to open a gap
            if constexpr (std::is_trivially_move_constructible_v<T>)
            {
                std::memmove(m_pData + index + count, m_pData + index, tail * sizeof(T));
            }
            else
            {
                for (size_type i = 0; i < tail; ++i)
                {
                    const size_type src = oldSize - 1 - i;
                    new (&m_pData[src + count]) T(std::move(m_pData[src]));
                    m_pData[src].~T();
                }
            }

            // constructed the inserted elements into the gap
            if constexpr (std::is_trivially_copyable_v<T> && IsElementPointer<InputIt>)
            {
                std::memcpy(m_pData + index, first, count * sizeof(T));
            }
            else
            {
                size_type w = index;

                for (InputIt it = first; it != last; ++it, ++w)
                {
                    new (&m_pData[w]) T(*it);
                }
            }

            m_size = oldSize + count;
        }
    }

    // Range appends grow geometrically like single appends; growing to the exact
    // size makes repeated appends copy the whole buffer each time.
    size_type grown_capacity(size_type required) const
    {
        const size_type doubled = m_capacity * 2;
        return required <= m_capacity ? m_capacity : required > doubled ? required : doubled;
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
            {
                ZEN_MEM_FREE(m_pData);
            }

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

    // Trivial element types skip per-element loops, which unoptimized builds
    // otherwise execute once per byte of large pixel and geometry payloads.
    void destroy_range(size_type begin, size_type end)
    {
        if constexpr (!std::is_trivially_destructible_v<T>)
        {
            for (size_type i = begin; i < end; ++i)
            {
                m_pData[i].~T();
            }
        }
    }

    void construct_range(size_type begin, size_type end)
    {
        if constexpr (std::is_trivially_default_constructible_v<T>)
        {
            // Value-initializing a trivial type zero-fills it.
            if (end > begin)
            {
                std::memset(static_cast<void*>(m_pData + begin), 0, sizeof(T) * (end - begin));
            }
        }
        else
        {
            for (size_type i = begin; i < end; ++i)
            {
                new (&m_pData[i]) T();
            }
        }
    }

    void move_from(HeapVector& other)
    {
        m_pData          = other.m_pData;
        m_size           = other.m_size;
        m_capacity       = other.m_capacity;

        other.m_pData    = nullptr;
        other.m_size     = 0;
        other.m_capacity = 0;
    }

    void copy_from(const HeapVector& other)
    {
        if (!other.empty())
        {
            // Copies are sized to their contents; geometric growth headroom stays with the source.
            const size_t bytes = other.m_size * sizeof(T);
            m_pData            = static_cast<T*>(ZEN_MEM_ALLOC_ALIGNED(bytes, alignof(T)));
            m_capacity         = other.m_size;

            if constexpr (std::is_trivially_copy_constructible_v<T>)
            {
                std::memcpy(m_pData, other.m_pData, other.m_size * sizeof(T));
            }
            else
            {
                for (size_type i = 0; i < other.m_size; i++)
                {
                    new (&m_pData[i]) T(other.m_pData[i]);
                }
            }

            m_size = other.m_size;
        }
    }
};

} // namespace zen
