#pragma once
#include <vector>
#include <cstdlib>
#include <exception>
#include <algorithm>
#include "Templates/HeapVector.h"

namespace zen
{
template <class T, size_t N> class AlignedBuffer
{
public:
    const T* data() const
    {
        return reinterpret_cast<T*>(m_alignedChar);
    }
    T* data()
    {
        return reinterpret_cast<T*>(m_alignedChar);
    }

private:
    alignas(T) char m_alignedChar[sizeof(T) * N];
};

template <typename T> class AlignedBuffer<T, 0>
{
public:
    const T* data() const
    {
        return nullptr;
    }
    T* data()
    {
        return nullptr;
    }
};

// An immutable version of SmallVector which erases type information about storage.
template <typename T> class VectorView
{
public:
    T& operator[](size_t i)
    {
        return m_pData[i];
    }

    const T& operator[](size_t i) const
    {
        return m_pData[i];
    }

    bool empty() const
    {
        return m_size == 0;
    }

    size_t size() const
    {
        return m_size;
    }

    T* data()
    {
        return m_pData;
    }

    const T* data() const
    {
        return m_pData;
    }

    T* begin()
    {
        return m_pData;
    }

    T* end()
    {
        return m_pData + m_size;
    }

    const T* begin() const
    {
        return m_pData;
    }

    const T* end() const
    {
        return m_pData + m_size;
    }

    T& front()
    {
        return m_pData[0];
    }

    const T& front() const
    {
        return m_pData[0];
    }

    T& back()
    {
        return m_pData[m_size - 1];
    }

    const T& back() const
    {
        return m_pData[m_size - 1];
    }

    // Avoid sliced copies. Base class should only be read as a reference.
    VectorView(const VectorView&) = default;

    VectorView& operator=(const VectorView&) = default;

    VectorView() = default;

    VectorView(const T& element) : m_pData(const_cast<T*>(&element)), m_size(1) {}

    VectorView(const T* pPtr, size_t size) : m_pData(pPtr), m_size(size) {}

    VectorView(T* pPtr, size_t size) : m_pData(pPtr), m_size(size) {}

    VectorView(const std::vector<T>& vec) : m_pData(const_cast<T*>(vec.data())), m_size(vec.size())
    {}

    VectorView(const HeapVector<T>& vec) : m_pData(const_cast<T*>(vec.data())), m_size(vec.size())
    {}

protected:
    T* m_pData{nullptr};
    size_t m_size{0};
};

template <class T, size_t N = 8> class SmallVector : public VectorView<T>
{
public:
    SmallVector()
    {
        this->m_pData = m_alignedBuffer.data();
        m_capacity    = N;
    }

    explicit SmallVector(size_t size) : SmallVector()
    {
        resize(size);
    }

    SmallVector(const SmallVector& other) : SmallVector()
    {
        *this = other;
    }

    SmallVector(SmallVector&& other) noexcept : SmallVector()
    {
        *this = std::move(other);
    }

    SmallVector(const T* pInsertBegin, const T* pInsertEnd) : SmallVector()
    {
        auto count = size_t(pInsertEnd - pInsertBegin);
        reserve(count);
        for (size_t i = 0; i < count; i++, pInsertBegin++)
        {
            new (&this->m_pData[i]) T(*pInsertBegin);
        }
        this->m_size = count;
    }

    SmallVector(const std::initializer_list<T>& initList) : SmallVector()
    {
        insert(this->end(), initList.begin(), initList.end());
    }

    ~SmallVector()
    {
        clear();
        if (this->m_pData != m_alignedBuffer.data())
            free(this->m_pData);
    }

    SmallVector& operator=(const SmallVector& other)
    {
        clear();
        reserve(other.m_size);
        for (size_t i = 0; i < other.m_size; i++)
            new (&this->m_pData[i]) T(other.m_pData[i]);
        this->m_size = other.m_size;
        return *this;
    }

    SmallVector& operator=(SmallVector&& other) noexcept
    {
        clear();
        if (other.m_pData != other.m_alignedBuffer.data())
        {
            // Pilfer allocated pointer.
            if (this->m_pData != m_alignedBuffer.data())
                free(this->m_pData);

            this->m_pData    = other.m_pData;
            this->m_size     = other.m_size;
            m_capacity       = other.m_capacity;
            other.m_pData    = nullptr;
            other.m_size     = 0;
            other.m_capacity = 0;
        }
        else
        {
            // Need to move the stack contents individually.
            reserve(other.m_size);
            for (size_t i = 0; i < other.m_size; i++)
            {
                new (&this->m_pData[i]) T(std::move(other.m_pData[i]));
                other.m_pData[i].~T();
            }
            this->m_size = other.m_size;
            other.m_size = 0;
        }
        return *this;
    }

    void push_back(const T& t)
    {
        reserve(this->m_size + 1);
        new (&this->m_pData[this->m_size]) T(t);
        ++this->m_size;
    }

    void push_back(T&& t)
    {
        reserve(this->m_size + 1);
        new (&this->m_pData[this->m_size]) T(std::move(t));
        ++this->m_size;
    }

    void pop_back()
    {
        if (!this->empty())
        {
            resize(this->m_size - 1);
        }
    }

    template <class... Args> void emplace_back(Args&&... ts)
    {
        reserve(this->m_size + 1);
        new (&this->m_pData[this->m_size]) T(std::forward<Args>(ts)...);
        ++this->m_size;
    }

    void reserve(size_t newCapacity)
    {
        if (newCapacity > m_capacity)
        {
            size_t targetCapacity = m_capacity;
            if (targetCapacity == 0)
            {
                targetCapacity = 1;
            }
            if (targetCapacity < N)
            {
                targetCapacity = N;
            }
            while (targetCapacity < newCapacity)
            {
                targetCapacity <<= 1u;
            }
            T* pNewBuffer = targetCapacity > N ?
                static_cast<T*>(malloc(targetCapacity * sizeof(T))) :
                m_alignedBuffer.data();
            if (!pNewBuffer)
            {
                std::terminate();
            }
            if (pNewBuffer != this->m_pData)
            {
                for (size_t i = 0; i < this->m_size; i++)
                {
                    new (&pNewBuffer[i]) T(std::move(this->m_pData[i]));
                    this->m_pData[i].~T();
                }
            }
            if (this->m_pData != m_alignedBuffer.data())
            {
                // reset array ptr
                free(this->m_pData);
            }
            this->m_pData = pNewBuffer;
            m_capacity    = targetCapacity;
        }
    }

    void insert(T* pIter, const T* pInsertBegin, const T* pInsertEnd)
    {
        auto count = size_t(pInsertEnd - pInsertBegin);
        if (pIter == this->end())
        {
            reserve(this->m_size + count);
            for (size_t i = 0; i < count; i++, pInsertBegin++)
            {
                // insert
                new (&this->m_pData[this->m_size + i]) T(*pInsertBegin);
            }
            this->m_size += count;
        }
        else
        {
            if (this->m_size + count > this->m_capacity)
            {
                size_t targetCapacity = this->m_size + count;
                if (targetCapacity == 0)
                {
                    targetCapacity = 1;
                }
                if (targetCapacity < N)
                {
                    targetCapacity = N;
                }
                while (targetCapacity < count)
                {
                    targetCapacity <<= 1u;
                }
                T* pNewBuffer = targetCapacity > N ?
                    static_cast<T*>(malloc(targetCapacity * sizeof(T))) :
                    m_alignedBuffer.data();
                if (!pNewBuffer)
                {
                    std::terminate();
                }

                // move elements (before insert position) from original source buffer to new buffer.
                auto* pTargetIter         = pNewBuffer;
                auto* pOriginalSourceIter = this->begin();

                if (pNewBuffer != this->m_pData)
                {
                    while (pOriginalSourceIter != pIter)
                    {
                        new (pTargetIter) T(std::move(*pOriginalSourceIter));
                        pOriginalSourceIter->~T();
                        ++pOriginalSourceIter;
                        ++pTargetIter;
                    }
                }
                // copy construct new elements
                for (auto* pSourceIter = pInsertBegin; pSourceIter != pInsertEnd;
                     ++pSourceIter, ++pTargetIter)
                {
                    new (pTargetIter) T(*pSourceIter);
                }
                // move the rest
                if (pNewBuffer != this->m_pData || pInsertBegin != pInsertEnd)
                {
                    while (pOriginalSourceIter != this->end())
                    {
                        new (pTargetIter) T(std::move(*pOriginalSourceIter));
                        pOriginalSourceIter->~T();
                        ++pOriginalSourceIter;
                        ++pTargetIter;
                    }
                }
                if (this->m_pData != m_alignedBuffer.data())
                {
                    // reset array ptr
                    free(this->m_pData);
                }
                this->m_pData = pNewBuffer;
                m_capacity    = targetCapacity;
            }
            else
            {
                auto* pTargetIter = this->end() + count;
                auto* pSourceIter = this->end();
                // move to the insertEnd, make space for new elements
                while (pTargetIter != this->end() && pSourceIter != pIter)
                {
                    --pTargetIter;
                    --pSourceIter;
                    new (pTargetIter) T(std::move(*pSourceIter));
                }

                std::move_backward(pIter, pSourceIter, pTargetIter);
                while (pIter != this->end() && pInsertBegin != pInsertEnd)
                {
                    *pIter++ = *pInsertBegin++;
                }
                while (pInsertBegin != pInsertEnd)
                {
                    new (pIter) T(*pInsertBegin);
                    ++pIter;
                    ++pInsertBegin;
                }
            }
            this->m_size += count;
        }
    }

    void insert(T* pItr, const T& value)
    {
        insert(pItr, &value, &value + 1);
    }

    T* erase(T* pItr)
    {
        std::move(pItr + 1, this->end(), pItr);
        this->m_pData[--this->m_size].~T();
        return pItr;
    }

    void erase(T* pStartErase, T* pEndErase)
    {
        if (pEndErase == this->end())
        {
            resize(size_t(pStartErase - this->begin()));
        }
        else
        {
            auto newSize = this->m_size - (pEndErase - pStartErase);
            std::move(pEndErase, this->end(), pStartErase);
            resize(newSize);
        }
    }


    void resize(size_t newSize)
    {
        if (newSize < this->m_size)
        {
            for (size_t i = newSize; i < this->m_size; i++)
            {
                this->m_pData[i].~T();
            }
        }
        else if (newSize > this->m_size)
        {
            reserve(newSize);
            for (size_t i = this->m_size; i < newSize; i++)
            {
                new (&this->m_pData[i]) T();
            }
        }
        this->m_size = newSize;
    }

    void clear()
    {
        for (size_t i = 0; i < this->m_size; i++)
        {
            this->m_pData[i].~T();
        }
        this->m_size = 0;
    }

private:
    size_t m_capacity{0};
    AlignedBuffer<T, N> m_alignedBuffer;
};

// Pointer + size
template <typename T> VectorView<T> MakeVecView(T* pPtr, size_t size)
{
    return VectorView<T>(pPtr, size);
}

// Const pointer + size
template <typename T> VectorView<const T> MakeVecView(const T* pPtr, size_t size)
{
    return VectorView<const T>(pPtr, size);
}

// C-array
template <typename T, size_t N> VectorView<T> MakeVecView(T (&arr)[N])
{
    return VectorView<T>(arr, N);
}

// const C-array
template <typename T, size_t N> VectorView<const T> MakeVecView(const T (&arr)[N])
{
    return VectorView<const T>(arr, N);
}

// std::vector
template <typename T> VectorView<T> MakeVecView(std::vector<T>& vec)
{
    return VectorView<T>(vec.data(), vec.size());
}

template <typename T> VectorView<const T> MakeVecView(const std::vector<T>& vec)
{
    return VectorView<const T>(vec.data(), vec.size());
}

// HeapVector
template <typename T> VectorView<T> MakeVecView(HeapVector<T>& vec)
{
    return VectorView<T>(vec.data(), vec.size());
}

template <typename T> VectorView<const T> MakeVecView(const HeapVector<T>& vec)
{
    return VectorView<const T>(vec.data(), vec.size());
}

// Single element
template <typename T> VectorView<T> MakeVecView(T& element)
{
    return VectorView<T>(&element, 1);
}

template <typename T> VectorView<const T> MakeVecView(const T& element)
{
    return VectorView<const T>(&element, 1);
}
} // namespace zen