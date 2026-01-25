#pragma once
#include "Templates/HeapVector.h"

namespace zen
{
template <typename T> class PoolAllocator
{
public:
    explicit PoolAllocator(size_t initialSize) : m_initialSize(initialSize)
    {
        AddNewAllocator(initialSize);
    }

    ~PoolAllocator()
    {
        for (T* alloc : m_allocators)
            delete alloc;
        m_allocators.clear();
    }

    // allocate memory from current allocator
    void* Alloc(size_t size, size_t alignment = alignof(std::max_align_t))
    {
        T* alloc  = m_allocators[m_currentIndex];
        void* mem = alloc->Alloc(size, alignment);

        if (mem != nullptr)
            return mem;

        // current allocator is full -> create a bigger one
        size_t newSize = alloc->Capacity() * 2;
        if (newSize < size)
            newSize = size * 2;

        AddNewAllocator(newSize);

        return m_allocators[m_currentIndex]->Alloc(size, alignment);
    }

    // reset all internal allocators (typically once per frame)
    void Reset()
    {
        for (T* alloc : m_allocators)
            alloc->Reset();
        m_currentIndex = 0;
    }

    size_t NumAllocators() const
    {
        return m_allocators.size();
    }

    T* CurrentAllocator()
    {
        return m_allocators[m_currentIndex];
    }

private:
    void AddNewAllocator(size_t size)
    {
        m_allocators.push_back(new T(size));
        m_currentIndex = m_allocators.size() - 1;
    }

private:
    HeapVector<T*> m_allocators;
    size_t m_currentIndex = 0;
    size_t m_initialSize;
};
} // namespace zen
