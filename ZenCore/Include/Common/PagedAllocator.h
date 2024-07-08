#pragma once
#include "Memory.h"
#include "SpinLock.h"

#define ZEN_DEFAULT_PAGESIZE 4096
namespace zen
{
static uint32_t CalcShiftFromPowerOf2(unsigned int num)
{
    for (uint32_t i = 0; i < 32; i++)
    {
        if (num == static_cast<uint32_t>(1 << i))
        {
            return i;
        }
    }

    return -1;
}

template <class T> class PagedAllocator
{
public:
    PagedAllocator(uint32_t pageSize, bool threadSafe) :
        m_pageSize(pageSize), m_threadSafe(threadSafe)
    {
        m_pageSize = Pow2Pad(pageSize);
    }

    void Init()
    {
        m_pageShift = CalcShiftFromPowerOf2(m_pageSize);
        m_pageMask  = m_pageSize - 1;
    }

    template <typename... Args> T* Alloc(Args&&... args)
    {
        if (m_threadSafe)
        {
            m_lock.Lock();
        }

        if (m_allocsAvailable == 0)
        {
            uint32_t pageIndex = m_numPagesAllocated;
            m_numPagesAllocated++;
            m_pagePool = static_cast<T**>(
                DefaultAllocator::Realloc(m_pagePool, sizeof(T*) * m_numPagesAllocated));
            m_freePages = static_cast<T***>(
                DefaultAllocator::Realloc(m_freePages, sizeof(T**) * m_numPagesAllocated));

            m_pagePool[pageIndex] =
                static_cast<T*>(DefaultAllocator::Alloc(sizeof(T) * m_pageSize));
            m_freePages[pageIndex] =
                static_cast<T**>(DefaultAllocator::Alloc(sizeof(T*) * m_pageSize));

            for (uint32_t i = 0; i < m_pageSize; i++)
            {
                m_freePages[pageIndex][i] = &m_pagePool[pageIndex][i];
            }
            m_allocsAvailable += m_pageSize;
        }
        m_allocsAvailable--;
        T* alloc = m_freePages[GetPageIndex()][GetSlotIndex()];
        new (alloc) T(args...);

        if (m_threadSafe)
        {
            m_lock.Unlock();
        }
        return alloc;
    }

    void Free(T* pMemory)
    {
        if (m_threadSafe)
        {
            m_lock.Lock();
        }

        pMemory->~T();
        m_freePages[GetPageIndex()][GetSlotIndex()] = pMemory;
        m_allocsAvailable++;

        if (m_threadSafe)
        {
            m_lock.Unlock();
        }
    }

private:
    uint32_t GetPageIndex() const
    {
        return m_allocsAvailable >> m_pageShift;
    }
    uint32_t GetSlotIndex() const
    {
        return m_allocsAvailable & m_pageMask;
    }

    // configurations
    uint32_t m_pageSize{ZEN_DEFAULT_PAGESIZE};
    // thread safe
    bool m_threadSafe{false};
    SpinLock m_lock;
    // page pool 2d array
    // pageIndex slot0 slot1 slot2 ... slot_(m_pageSize-1)
    //     0       T     T     T   ...   T
    //     1       T     T     T   ...   T
    //     2       T     T     T   ...   T
    T** m_pagePool{nullptr};
    // free pages 2d pointer array
    // pageIndex slot0 slot1 slot2  ... slot_(m_pageSize-1)
    //     0       T*    T*    T*   ...   T*
    //     1       T*    T*    T*   ...   T*
    //     2       T*    T*    T*   ...   T*
    T*** m_freePages{nullptr};

    uint32_t m_numPagesAllocated{0};
    uint32_t m_allocsAvailable{0};
    uint32_t m_pageShift{0};
    uint32_t m_pageMask{0};
};
} // namespace zen