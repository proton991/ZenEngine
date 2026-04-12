#pragma once
#include "Memory.h"
#include "Utils/SpinLock.h"
#include "Utils/Errors.h"

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

    ~PagedAllocator()
    {
        if (m_threadSafe)
        {
            m_lock.Lock();
        }

        bool leaked = m_allocsAvailable < m_numPagesAllocated * m_pageSize;
        if (leaked)
        {
            LOGE("Pages in use exist at exit in PagedAllocator");
        }
        else
        {
            Reset();
        }

        if (m_threadSafe)
        {
            m_lock.Unlock();
        }
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
            m_pPagePool =
                static_cast<T**>(ZEN_MEM_REALLOC(m_pPagePool, sizeof(T*) * m_numPagesAllocated));
            m_pFreePages =
                static_cast<T***>(ZEN_MEM_REALLOC(m_pFreePages, sizeof(T**) * m_numPagesAllocated));

            m_pPagePool[pageIndex]  = static_cast<T*>(ZEN_MEM_ALLOC(sizeof(T) * m_pageSize));
            m_pFreePages[pageIndex] = static_cast<T**>(ZEN_MEM_ALLOC(sizeof(T*) * m_pageSize));

            for (uint32_t i = 0; i < m_pageSize; i++)
            {
                m_pFreePages[pageIndex][i] = &m_pPagePool[pageIndex][i];
            }
            m_allocsAvailable += m_pageSize;
        }
        m_allocsAvailable--;
        T* pAlloc = m_pFreePages[GetPageIndex()][GetSlotIndex()];
        new (pAlloc) T(args...);

        if (m_threadSafe)
        {
            m_lock.Unlock();
        }
        return pAlloc;
    }

    void Free(T* pMemory)
    {
        if (m_threadSafe)
        {
            m_lock.Lock();
        }

        pMemory->~T();
        m_pFreePages[GetPageIndex()][GetSlotIndex()] = pMemory;
        m_allocsAvailable++;

        if (m_threadSafe)
        {
            m_lock.Unlock();
        }
    }

private:
    void Reset()
    {
        if (m_numPagesAllocated > 0)
        {
            for (uint32_t i = 0; i < m_numPagesAllocated; i++)
            {
                ZEN_MEM_FREE(m_pPagePool[i]);
                ZEN_MEM_FREE(m_pFreePages[i]);
            }
            ZEN_MEM_FREE(m_pPagePool);
            ZEN_MEM_FREE(m_pFreePages);
            m_pPagePool          = nullptr;
            m_pFreePages         = nullptr;
            m_numPagesAllocated = 0;
            m_allocsAvailable   = 0;
        }
    }

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
    T** m_pPagePool{nullptr};
    // free pages 2d pointer array
    // pageIndex slot0 slot1 slot2  ... slot_(m_pageSize-1)
    //     0       T*    T*    T*   ...   T*
    //     1       T*    T*    T*   ...   T*
    //     2       T*    T*    T*   ...   T*
    T*** m_pFreePages{nullptr};

    uint32_t m_numPagesAllocated{0};
    uint32_t m_allocsAvailable{0};
    uint32_t m_pageShift{0};
    uint32_t m_pageMask{0};
};
} // namespace zen