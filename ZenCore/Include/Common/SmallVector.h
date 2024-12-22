#pragma once
#include <vector>
#include <cstdlib>
#include <exception>
#include <algorithm>
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
        return m_ptr[i];
    }

    const T& operator[](size_t i) const
    {
        return m_ptr[i];
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
        return m_ptr;
    }

    const T* data() const
    {
        return m_ptr;
    }

    T* begin()
    {
        return m_ptr;
    }

    T* end()
    {
        return m_ptr + m_size;
    }

    const T* begin() const
    {
        return m_ptr;
    }

    const T* end() const
    {
        return m_ptr + m_size;
    }

    T& front()
    {
        return m_ptr[0];
    }

    const T& front() const
    {
        return m_ptr[0];
    }

    T& back()
    {
        return m_ptr[m_size - 1];
    }

    const T& back() const
    {
        return m_ptr[m_size - 1];
    }

    // Avoid sliced copies. Base class should only be read as a reference.
    VectorView(const VectorView&) = delete;

    void operator=(const VectorView&) = delete;

    VectorView() = default;

    VectorView(const T& element) : m_ptr(const_cast<T*>(&element)), m_size(1) {}

    VectorView(const T* ptr, size_t size) : m_ptr(ptr), m_size(size) {}

    VectorView(T* ptr, size_t size) : m_ptr(ptr), m_size(size) {}

    VectorView(const std::vector<T>& vec) : m_ptr(const_cast<T*>(vec.data())), m_size(vec.size()) {}

protected:
    T* m_ptr{nullptr};
    size_t m_size{0};
};

template <class T, size_t N = 8> class SmallVector : public VectorView<T>
{
public:
    SmallVector()
    {
        this->m_ptr = m_alignedBuffer.data();
        m_capacity  = N;
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

    SmallVector(const T* insertBegin, const T* insertEnd) : SmallVector()
    {
        auto count = size_t(insertEnd - insertBegin);
        reserve(count);
        for (size_t i = 0; i < count; i++, insertBegin++)
        {
            new (&this->m_ptr[i]) T(*insertBegin);
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
        if (this->m_ptr != m_alignedBuffer.data())
            free(this->m_ptr);
    }

    SmallVector& operator=(const SmallVector& other)
    {
        clear();
        reserve(other.m_size);
        for (size_t i = 0; i < other.m_size; i++)
            new (&this->m_ptr[i]) T(other.m_ptr[i]);
        this->m_size = other.m_size;
        return *this;
    }

    SmallVector& operator=(SmallVector&& other) noexcept
    {
        clear();
        if (other.m_ptr != other.m_alignedBuffer.data())
        {
            // Pilfer allocated pointer.
            if (this->m_ptr != m_alignedBuffer.data())
                free(this->m_ptr);

            this->m_ptr      = other.m_ptr;
            this->m_size     = other.m_size;
            m_capacity       = other.m_capacity;
            other.m_ptr      = nullptr;
            other.m_size     = 0;
            other.m_capacity = 0;
        }
        else
        {
            // Need to move the stack contents individually.
            reserve(other.m_size);
            for (size_t i = 0; i < other.m_size; i++)
            {
                new (&this->m_ptr[i]) T(std::move(other.m_ptr[i]));
                other.m_ptr[i].~T();
            }
            this->m_size = other.m_size;
            other.m_size = 0;
        }
        return *this;
    }

    void push_back(const T& t)
    {
        reserve(this->m_size + 1);
        new (&this->m_ptr[this->m_size]) T(t);
        ++this->m_size;
    }

    void push_back(T&& t)
    {
        reserve(this->m_size + 1);
        new (&this->m_ptr[this->m_size]) T(std::move(t));
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
        new (&this->m_ptr[this->m_size]) T(std::forward<Args>(ts)...);
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
            T* newBuffer = targetCapacity > N ?
                static_cast<T*>(malloc(targetCapacity * sizeof(T))) :
                m_alignedBuffer.data();
            if (!newBuffer)
            {
                std::terminate();
            }
            if (newBuffer != this->m_ptr)
            {
                for (size_t i = 0; i < this->m_size; i++)
                {
                    new (&newBuffer[i]) T(std::move(this->m_ptr[i]));
                    this->m_ptr[i].~T();
                }
            }
            if (this->m_ptr != m_alignedBuffer.data())
            {
                // reset array ptr
                free(this->m_ptr);
            }
            this->m_ptr = newBuffer;
            m_capacity  = targetCapacity;
        }
    }

    void insert(T* iter, const T* insertBegin, const T* insertEnd)
    {
        auto count = size_t(insertEnd - insertBegin);
        if (iter == this->end())
        {
            reserve(this->m_size + count);
            for (size_t i = 0; i < count; i++, insertBegin++)
            {
                // insert
                new (&this->m_ptr[this->m_size + i]) T(*insertBegin);
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
                T* newBuffer = targetCapacity > N ?
                    static_cast<T*>(malloc(targetCapacity * sizeof(T))) :
                    m_alignedBuffer.data();
                if (!newBuffer)
                {
                    std::terminate();
                }

                // move elements (before insert position) from original source buffer to new buffer.
                auto* targetIter         = newBuffer;
                auto* originalSourceIter = this->begin();

                if (newBuffer != this->m_ptr)
                {
                    while (originalSourceIter != iter)
                    {
                        new (targetIter) T(std::move(*originalSourceIter));
                        originalSourceIter->~T();
                        ++originalSourceIter;
                        ++targetIter;
                    }
                }
                // copy construct new elements
                for (auto* sourceIter = insertBegin; sourceIter != insertEnd;
                     ++sourceIter, ++targetIter)
                {
                    new (targetIter) T(*sourceIter);
                }
                // move the rest
                if (newBuffer != this->m_ptr || insertBegin != insertEnd)
                {
                    while (originalSourceIter != this->end())
                    {
                        new (targetIter) T(std::move(*originalSourceIter));
                        originalSourceIter->~T();
                        ++originalSourceIter;
                        ++targetIter;
                    }
                }
                if (this->m_ptr != m_alignedBuffer.data())
                {
                    // reset array ptr
                    free(this->m_ptr);
                }
                this->m_ptr = newBuffer;
                m_capacity  = targetCapacity;
            }
            else
            {
                auto* targetIter = this->end() + count;
                auto* sourceIter = this->end();
                // move to the insertEnd, make space for new elements
                while (targetIter != this->end() && sourceIter != iter)
                {
                    --targetIter;
                    --sourceIter;
                    new (targetIter) T(std::move(*sourceIter));
                }

                std::move_backward(iter, sourceIter, targetIter);
                while (iter != this->end() && insertBegin != insertEnd)
                {
                    *iter++ = *insertBegin++;
                }
                while (insertBegin != insertEnd)
                {
                    new (iter) T(*insertBegin);
                    ++iter;
                    ++insertBegin;
                }
            }
            this->m_size += count;
        }
    }

    void insert(T* itr, const T& value)
    {
        insert(itr, &value, &value + 1);
    }

    T* erase(T* itr)
    {
        std::move(itr + 1, this->end(), itr);
        this->m_ptr[--this->m_size].~T();
        return itr;
    }

    void erase(T* startErase, T* endErase)
    {
        if (endErase == this->end())
        {
            resize(size_t(startErase - this->begin()));
        }
        else
        {
            auto newSize = this->m_size - (endErase - startErase);
            std::move(endErase, this->end(), startErase);
            resize(newSize);
        }
    }


    void resize(size_t newSize)
    {
        if (newSize < this->m_size)
        {
            for (size_t i = newSize; i < this->m_size; i++)
            {
                this->m_ptr[i].~T();
            }
        }
        else if (newSize > this->m_size)
        {
            reserve(newSize);
            for (size_t i = this->m_size; i < newSize; i++)
            {
                new (&this->m_ptr[i]) T();
            }
        }
        this->m_size = newSize;
    }

    void clear()
    {
        for (size_t i = 0; i < this->m_size; i++)
        {
            this->m_ptr[i].~T();
        }
        this->m_size = 0;
    }

private:
    size_t m_capacity{0};
    AlignedBuffer<T, N> m_alignedBuffer;
};
} // namespace zen