#pragma once

#include <cstdlib>
#include <exception>
#include <algorithm>
namespace zen
{
template <class T, size_t N> class AlignedBuffer
{
public:
    const T* Data() const { return reinterpret_cast<T*>(m_alignedChar); }
    T*       Data() { return reinterpret_cast<T*>(m_alignedChar); }

private:
    alignas(T) char m_alignedChar[sizeof(T) * N];
};

template <typename T> class AlignedBuffer<T, 0>
{
public:
    const T* Data() const { return nullptr; }
    T*       Data() { return nullptr; }
};

// An immutable version of SmallVector which erases type information about storage.
template <typename T> class VectorView
{
public:
    T& operator[](size_t i) { return m_ptr[i]; }

    const T& operator[](size_t i) const { return m_ptr[i]; }

    bool Empty() const { return m_size == 0; }

    size_t Size() const { return m_size; }

    T* Data() { return m_ptr; }

    const T* Data() const { return m_ptr; }

    T* Begin() { return m_ptr; }

    T* End() { return m_ptr + m_size; }

    const T* Begin() const { return m_ptr; }

    const T* End() const { return m_ptr + m_size; }

    T& Front() { return m_ptr[0]; }

    const T& Front() const { return m_ptr[0]; }

    T& Back() { return m_ptr[m_size - 1]; }

    const T& Back() const { return m_ptr[m_size - 1]; }

    // Avoid sliced copies. Base class should only be read as a reference.
    VectorView(const VectorView&)     = delete;
    void operator=(const VectorView&) = delete;

protected:
    VectorView() = default;
    T*     m_ptr{nullptr};
    size_t m_size{0};
};

template <class T, size_t N = 8> class SmallVector : public VectorView<T>
{
public:
    SmallVector()
    {
        this->m_ptr = m_alignedBuffer.Data();
        m_capacity  = N;
    }

    explicit SmallVector(size_t size) : SmallVector() { Resize(size); }

    SmallVector(const SmallVector& other) : SmallVector() { *this = other; }

    SmallVector(SmallVector&& other) noexcept : SmallVector() { *this = std::move(other); }

    SmallVector(const T* insertBegin, const T* insertEnd) : SmallVector()
    {
        auto count = size_t(insertEnd - insertBegin);
        Reserve(count);
        for (size_t i = 0; i < count; i++, insertBegin++) { new (&this->m_ptr[i]) T(*insertBegin); }
        this->m_size = count;
    }

    SmallVector(const std::initializer_list<T>& initList) : SmallVector()
    {
        Insert(this->End(), initList.begin(), initList.end());
    }

    ~SmallVector()
    {
        Clear();
        if (this->m_ptr != m_alignedBuffer.Data()) free(this->m_ptr);
    }

    SmallVector& operator=(const SmallVector& other)
    {
        Clear();
        Reserve(other.m_size);
        for (size_t i = 0; i < other.m_size; i++) new (&this->m_ptr[i]) T(other.m_ptr[i]);
        this->m_size = other.m_size;
        return *this;
    }

    SmallVector& operator=(SmallVector&& other) noexcept
    {
        Clear();
        if (other.m_ptr != other.m_alignedBuffer.Data())
        {
            // Pilfer allocated pointer.
            if (this->m_ptr != m_alignedBuffer.Data()) free(this->m_ptr);

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
            Reserve(other.m_size);
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

    void PushBack(const T& t)
    {
        Reserve(this->m_size + 1);
        new (&this->m_ptr[this->m_size]) T(t);
        this->m_size++;
    }

    void PushBack(T&& t)
    {
        Reserve(this->m_size + 1);
        new (&this->m_ptr[this->m_size]) T(std::move(t));
        this->m_size++;
    }

    void PopBack()
    {
        if (!this->Empty()) { Resize(this->m_size - 1); }
    }

    template <class... Args> void EmplaceBack(Args&&... ts)
    {
        Reserve(this->m_size + 1);
        new (&this->m_ptr[this->m_size]) T(std::forward<Args>(ts)...);
        this->m_size++;
    }

    void Reserve(size_t newCapacity)
    {
        if (newCapacity > m_capacity)
        {
            size_t targetCapacity = m_capacity;
            if (targetCapacity == 0) { targetCapacity = 1; }
            if (targetCapacity < N) { targetCapacity = N; }
            while (targetCapacity < newCapacity) { targetCapacity <<= 1u; }
            T* newBuffer = targetCapacity > N ?
                static_cast<T*>(malloc(targetCapacity * sizeof(T))) :
                m_alignedBuffer.Data();
            if (!newBuffer) { std::terminate(); }
            if (newBuffer != this->m_ptr)
            {
                for (size_t i = 0; i < this->m_size; i++)
                {
                    new (&newBuffer[i]) T(std::move(this->m_ptr[i]));
                    this->m_ptr->~T();
                }
            }
            if (this->m_ptr != m_alignedBuffer.Data())
            {
                // reset array ptr
                free(this->m_ptr);
            }
            this->m_ptr = newBuffer;
            m_capacity  = targetCapacity;
        }
    }

    void Insert(T* iter, const T* insertBegin, const T* insertEnd)
    {
        auto count = size_t(insertEnd - insertBegin);
        if (iter == this->End())
        {
            Reserve(this->m_size + count);
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
                if (targetCapacity == 0) { targetCapacity = 1; }
                if (targetCapacity < N) { targetCapacity = N; }
                while (targetCapacity < count) { targetCapacity <<= 1u; }
                T* newBuffer = targetCapacity > N ?
                    static_cast<T*>(malloc(targetCapacity * sizeof(T))) :
                    m_alignedBuffer.Data();
                if (!newBuffer) { std::terminate(); }

                // move elements (before insert position) from original source buffer to new buffer.
                auto* targetIter         = newBuffer;
                auto* originalSourceIter = this->Begin();

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
                    while (originalSourceIter != this->End())
                    {
                        new (targetIter) T(std::move(*originalSourceIter));
                        originalSourceIter->~T();
                        ++originalSourceIter;
                        ++targetIter;
                    }
                }
                if (this->m_ptr != m_alignedBuffer.Data())
                {
                    // reset array ptr
                    free(this->m_ptr);
                }
                this->m_ptr = newBuffer;
                m_capacity  = targetCapacity;
            }
            else
            {
                auto* targetIter = this->End() + count;
                auto* sourceIter = this->End();
                // move to the insertEnd, make space for new elements
                while (targetIter != this->End() && sourceIter != iter)
                {
                    --targetIter;
                    --sourceIter;
                    new (targetIter) T(std::move(*sourceIter));
                }

                std::move_backward(iter, sourceIter, targetIter);
                while (iter != this->End() && insertBegin != insertEnd)
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

    void Insert(T* itr, const T& value) { Insert(itr, &value, &value + 1); }

    T* Erase(T* itr)
    {
        std::move(itr + 1, this->End(), itr);
        this->m_ptr[--this->m_size].~T();
        return itr;
    }

    void erase(T* startErase, T* endErase)
    {
        if (endErase == this->End()) { Resize(size_t(startErase - this->Begin())); }
        else
        {
            auto newSize = this->m_size - (endErase - startErase);
            std::move(endErase, this->End(), startErase);
            resize(newSize);
        }
    }


    void Resize(size_t newSize)
    {
        if (newSize < this->m_size)
        {
            for (size_t i = newSize; i < this->m_size; i++) { this->m_ptr[i].~T(); }
        }
        else if (newSize > this->m_size) { Reserve(newSize); }
        this->m_size = newSize;
    }

    void Clear()
    {
        for (size_t i = 0; i < this->m_size; i++) { this->m_ptr[i].~T(); }
        this->m_size = 0;
    }

private:
    size_t              m_capacity{0};
    AlignedBuffer<T, N> m_alignedBuffer;
};
} // namespace zen