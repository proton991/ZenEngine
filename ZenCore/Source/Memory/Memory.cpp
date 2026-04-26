#include "Memory/Memory.h"
#include "Utils/Errors.h"

#include <algorithm>
#include <cstdint>

namespace zen
{
struct MemoryFreeHelper
{
    ~MemoryFreeHelper()
    {
        DefaultAllocator::ReportMemUsage();
    }
};

MemoryFreeHelper g_memoryFreeHelper;

DefaultAllocator* DefaultAllocator::GetInstance()
{
    static DefaultAllocator instance;
    return &instance;
}

void DefaultAllocator::LockAllocationSiteStats()
{
    while (m_allocationSiteStatsLock.test_and_set(std::memory_order_acquire))
    {
    }
}

void DefaultAllocator::UnlockAllocationSiteStats()
{
    m_allocationSiteStatsLock.clear(std::memory_order_release);
}

DefaultAllocator::AllocationSiteStats* DefaultAllocator::FindOrAddAllocationSite(
    const char* pFileName,
    uint32_t lineNum)
{
    uintptr_t hash = reinterpret_cast<uintptr_t>(pFileName);
    hash ^= static_cast<uintptr_t>(lineNum) * 2654435761u;
    uint32_t index = static_cast<uint32_t>(hash % cMaxTrackedAllocationSites);

    for (uint32_t probe = 0; probe < cMaxTrackedAllocationSites; ++probe)
    {
        AllocationSiteStats& stats = m_allocationSiteStats[index];
        if (stats.pFileName == pFileName && stats.lineNumber == lineNum)
        {
            return &stats;
        }

        if (stats.pFileName == nullptr)
        {
            stats.pFileName  = pFileName;
            stats.lineNumber = lineNum;
            return &stats;
        }

        index = (index + 1) % cMaxTrackedAllocationSites;
    }

    m_allocationSiteStatsOverflow = true;
    return nullptr;
}

void DefaultAllocator::TrackAllocationSiteAlloc(size_t size,
                                                const char* pFileName,
                                                uint32_t lineNum,
                                                bool isRealloc)
{
    if (pFileName == nullptr || size == 0)
    {
        return;
    }

    LockAllocationSiteStats();
    AllocationSiteStats* pStats = FindOrAddAllocationSite(pFileName, lineNum);
    if (pStats != nullptr)
    {
        pStats->totalAllocated += size;
        pStats->currentUsage += size;
        pStats->peakUsage = std::max(pStats->peakUsage, pStats->currentUsage);
        if (isRealloc)
        {
            ++pStats->reallocCount;
        }
        else
        {
            ++pStats->allocationCount;
        }
    }
    UnlockAllocationSiteStats();
}

void DefaultAllocator::TrackAllocationSiteFree(size_t size,
                                               const char* pFileName,
                                               uint32_t lineNum)
{
    if (pFileName == nullptr || size == 0)
    {
        return;
    }

    LockAllocationSiteStats();
    AllocationSiteStats* pStats = FindOrAddAllocationSite(pFileName, lineNum);
    if (pStats != nullptr)
    {
        pStats->totalFreed += size;
        pStats->currentUsage -= std::min(pStats->currentUsage, size);
        ++pStats->freeCount;
    }
    UnlockAllocationSiteStats();
}

void DefaultAllocator::TrackMemAlloc(size_t s, const char* pFileName, uint32_t lineNum)
{
    DefaultAllocator* pAllocator = GetInstance();
    pAllocator->m_totalAllocated += s;
    pAllocator->m_currentUsage += s;

    size_t peak = pAllocator->m_peakUsage.load();
    while (pAllocator->m_currentUsage > peak &&
           !pAllocator->m_peakUsage.compare_exchange_weak(peak, pAllocator->m_currentUsage))
    {
    }

    pAllocator->TrackAllocationSiteAlloc(s, pFileName, lineNum, false);
}

void DefaultAllocator::TrackMemReAlloc(size_t oldSize,
                                       size_t newSize,
                                       const char* pFileName,
                                       uint32_t lineNum)
{
    DefaultAllocator* pAllocator = GetInstance();

    if (newSize > oldSize)
    {
        const size_t sizeDelta = newSize - oldSize;
        pAllocator->m_totalAllocated += sizeDelta;
        pAllocator->m_currentUsage += sizeDelta;
        pAllocator->TrackAllocationSiteAlloc(sizeDelta, pFileName, lineNum, true);
    }
    else
    {
        const size_t sizeDelta = oldSize - newSize;
        pAllocator->m_totalFreed += sizeDelta;
        pAllocator->m_currentUsage -= sizeDelta;
        pAllocator->TrackAllocationSiteFree(sizeDelta, pFileName, lineNum);
    }

    size_t peak = pAllocator->m_peakUsage.load();
    while (pAllocator->m_currentUsage > peak &&
           !pAllocator->m_peakUsage.compare_exchange_weak(peak, pAllocator->m_currentUsage))
    {
    }
}

void DefaultAllocator::TrackMemFree(size_t s, const char* pFileName, uint32_t lineNum)
{
    DefaultAllocator* pAllocator = GetInstance();
    pAllocator->m_totalFreed += s;
    pAllocator->m_currentUsage -= s;
    pAllocator->TrackAllocationSiteFree(s, pFileName, lineNum);
}

void* DefaultAllocator::Alloc(size_t s, size_t alignment, const char* pFileName, uint32_t lineNum)
{
#if defined(ZEN_DEBUG)
    const size_t totalSize = sizeof(AllocationHeader) + s;
    AllocationHeader* pHeader =
        static_cast<AllocationHeader*>(DefaultAllocImpl(totalSize, alignment));

    ASSERT(pHeader != nullptr);

    pHeader->size_      = s;
    pHeader->pFileName  = pFileName;
    pHeader->lineNumber = lineNum;

    TrackMemAlloc(s, pFileName, lineNum);

    return pHeader + 1;
#else
    void* pMemory = DefaultAllocImpl(s, alignment);
    return pMemory;
#endif
}

void* DefaultAllocator::Calloc(size_t s, size_t alignment, const char* pFileName, uint32_t lineNumm)
{
    void* pMem = Alloc(s, alignment, pFileName, lineNumm);
    std::memset(pMem, 0, s);
    return pMem;
}

void DefaultAllocator::Free(void* pMemory, const char* pFileName, uint32_t lineNumm)
{
    if (!pMemory)
        return;

#if defined(ZEN_DEBUG)
    auto* pHeader = static_cast<AllocationHeader*>(pMemory) - 1;

    TrackMemFree(pHeader->size_, pHeader->pFileName, pHeader->lineNumber);

    DefaultFreeImpl(pHeader);
#else
    DefaultFreeImpl(pMemory);
#endif
}

void* DefaultAllocator::Realloc(void* pMem,
                                size_t newSize,
                                size_t alignment,
                                const char* pFileName,
                                uint32_t lineNumm)
{
#if defined(ZEN_DEBUG)
    if (!pMem)
        return Alloc(newSize, alignment, pFileName, lineNumm);

    auto* pOldHeader = reinterpret_cast<AllocationHeader*>(pMem) - 1;
    size_t oldSize   = pOldHeader->size_;
    const char* pOldFileName = pOldHeader->pFileName;
    const uint32_t oldLineNum = pOldHeader->lineNumber;

    const size_t totalSize = sizeof(AllocationHeader) + newSize;
    void* pRaw              = DefaultReallocImpl(pOldHeader, totalSize, alignment,
                                   sizeof(AllocationHeader) + std::min(oldSize, newSize));
    assert(pRaw);

    auto* pNewHeader  = reinterpret_cast<AllocationHeader*>(pRaw);
    pNewHeader->size_ = newSize;

    TrackMemReAlloc(oldSize, newSize, pOldFileName, oldLineNum);

    return pNewHeader + 1;
#else
    return DefaultReallocImpl(pMem, newSize, alignment);
#endif
}

static double BytesToMB(size_t bytes)
{
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

static void PrintMemorySize(size_t bytes)
{
    constexpr double KB = 1024.0;
    constexpr double MB = KB * 1024.0;
    constexpr double GB = MB * 1024.0;

    if (bytes < KB)
    {
        printf("%zu B", bytes);
    }
    else if (bytes < MB)
    {
        printf("%.2f KB", bytes / KB);
    }
    else if (bytes < GB)
    {
        printf("%.2f MB", bytes / MB);
    }
    else
    {
        printf("%.2f GB", bytes / GB);
    }
}

static void PrintMemoryLine(const char* pLabel, size_t bytes)
{
    printf("%-16s : ", pLabel);
    PrintMemorySize(bytes);
    printf("\n");
}

void DefaultAllocator::ReportMemUsage()
{
    DefaultAllocator* pAlloc = GetInstance();

    const size_t totalAllocated = pAlloc->m_totalAllocated.load();
    const size_t totalFreed     = pAlloc->m_totalFreed.load();
    const size_t currentUsage   = pAlloc->m_currentUsage.load();
    const size_t peakUsage      = pAlloc->m_peakUsage.load();

    printf("\n========== DefaultAllocator Memory Report ==========\n");

    PrintMemoryLine("Total Allocated", totalAllocated);
    PrintMemoryLine("Total Freed", totalFreed);
    PrintMemoryLine("Current Usage", currentUsage);
    PrintMemoryLine("Peak Usage", peakUsage);
    printf("\nNote: Total Allocated / Total Freed are lifetime counters, not current live memory.\n");

    constexpr uint32_t cNumTopAllocationSites = 16;
    AllocationSiteStats topSites[cNumTopAllocationSites]{};
    uint32_t numTopSites = 0;

    pAlloc->LockAllocationSiteStats();
    for (const AllocationSiteStats& stats : pAlloc->m_allocationSiteStats)
    {
        if (stats.pFileName == nullptr || stats.totalAllocated == 0)
        {
            continue;
        }

        if (numTopSites < cNumTopAllocationSites)
        {
            topSites[numTopSites] = stats;
            uint32_t siteIndex    = numTopSites++;
            while (siteIndex > 0 &&
                   topSites[siteIndex].totalAllocated > topSites[siteIndex - 1].totalAllocated)
            {
                std::swap(topSites[siteIndex], topSites[siteIndex - 1]);
                --siteIndex;
            }
        }
        else if (stats.totalAllocated > topSites[cNumTopAllocationSites - 1].totalAllocated)
        {
            topSites[cNumTopAllocationSites - 1] = stats;
            uint32_t siteIndex = cNumTopAllocationSites - 1;
            while (siteIndex > 0 &&
                   topSites[siteIndex].totalAllocated > topSites[siteIndex - 1].totalAllocated)
            {
                std::swap(topSites[siteIndex], topSites[siteIndex - 1]);
                --siteIndex;
            }
        }
    }
    const bool allocationSiteStatsOverflow = pAlloc->m_allocationSiteStatsOverflow;
    pAlloc->UnlockAllocationSiteStats();

    if (numTopSites > 0)
    {
        printf("\nTop Allocation Sites by Total Allocated:\n");
        for (uint32_t i = 0; i < numTopSites; ++i)
        {
            const AllocationSiteStats& stats = topSites[i];
            printf("#%02u  total=", i + 1);
            PrintMemorySize(stats.totalAllocated);
            printf(" allocs=%zu reallocs=%zu freed=", stats.allocationCount, stats.reallocCount);
            PrintMemorySize(stats.totalFreed);
            printf(" peakLive=");
            PrintMemorySize(stats.peakUsage);
            printf(" current=");
            PrintMemorySize(stats.currentUsage);
            printf("\n      at %s:%u\n", stats.pFileName, stats.lineNumber);
        }

        if (allocationSiteStatsOverflow)
        {
            printf("[WARN] Allocation site table overflowed; increase "
                   "DefaultAllocator::cMaxTrackedAllocationSites for complete stats.\n");
        }
    }

    if (currentUsage != 0)
    {
        printf("\n[ERROR] MEMORY LEAK DETECTED: ");
        PrintMemorySize(currentUsage);
        printf("\n");
    }
    else
    {
        printf("\n[OK] No memory leaks detected\n");
    }

    printf("====================================================\n");
}

// void DefaultAllocator::ReportMemUsage()
// {
//     DefaultAllocator* pAllocator = GetInstance();
//
//     printf("========== DefaultAllocator Memory Report ==========\n");
//     printf("Total Allocated : %.2f MB\n", BytesToMB(pAllocator->m_totalAllocated));
//     printf("Total Freed     : %.2f MB\n", BytesToMB(pAllocator->m_totalFreed));
//     printf("Current Usage   : %.2f MB\n", BytesToMB(pAllocator->m_currentUsage));
//     printf("Peak Usage      : %.2f MB\n", BytesToMB(pAllocator->m_peakUsage));
//
//     size_t currentUsage = pAllocator->m_currentUsage.load();
//     if (currentUsage != 0)
//     {
//         printf("[ERROR] MEMORY LEAK DETECTED: %.2f MB\n", BytesToMB(currentUsage));
//     }
//     else
//     {
//         printf("[OK] Everything is fine\n]");
//     }
//     printf("====================================================");
// }

void* DefaultAllocator::DefaultAllocImpl(size_t size, size_t alignment)
{
    void* pMem;
#if _POSIX_VERSION >= 20112L || defined(ZEN_MACOS)
    if (posix_memalign(&pMem, Pow2Align(alignment, sizeof(void*)), size) != 0)
    {
        pMem = nullptr;
    }
#elif _MSC_VER
    pMem = _aligned_malloc(size, Pow2Pad(size));
#else
#    error "Unsuported Platform"
#endif
    // GetAllocations()[pMem] = size;
    return pMem;
}

void* DefaultAllocator::DefaultCallocImpl(size_t size, size_t alignment)
{
    void* pMem = DefaultAllocImpl(size, alignment);
    if (pMem)
    {
        std::memset(pMem, 0, size);
    }
    // GetAllocations()[pMem] = size;
    return pMem;
}

void* DefaultAllocator::DefaultReallocImpl(void* pMem,
                                           size_t size,
                                           size_t alignment,
                                           size_t copySize)
{
    // nullptr do alloc
    if (pMem == nullptr)
    {
        return DefaultAllocImpl(size, alignment);
    }

    void* pNewMem;
#if _POSIX_VERSION >= 200112L || defined(ZEN_MACOS)
    // posix_memalign does not support realloc, so we need to manually handle it
    pNewMem = DefaultAllocImpl(size, alignment);
    if (pNewMem)
    {
        const size_t bytesToCopy = copySize > 0 ? copySize : size;
        std::memcpy(pNewMem, pMem, bytesToCopy);
        DefaultFreeImpl(pMem);
    }
#elif defined(_MSC_VER)
    pNewMem = _aligned_realloc(pMem, size, Pow2Pad(alignment));
#else
#    error "Unsupported Platform"
#endif
    return pNewMem;
}

void DefaultAllocator::DefaultFreeImpl(void* pMem)
{
    // if (!GetAllocations().contains(pMem))
    // {
    //     LOGE("Untracked meory allocation in DefaultAllocator!");
    // }
    // else
    // {
    //     GetAllocations().erase(pMem);
    // }
#if _POSIX_VERSION >= 20112L || defined(ZEN_MACOS)
    free(pMem);
#elif _MSC_VER
    _aligned_free(pMem);
#else
#    error "Unsuported Platform"
#endif
}
} // namespace zen

// void* operator new(std::size_t size)
// {
//     void* ptr = DefaultAllocator::Alloc(size);
//     if (!ptr)
//     {
//         throw std::bad_alloc();
//     }
//     return ptr;
// }
//
// void* operator new(std::size_t size, std::align_val_t align)
// {
//     void* ptr = DefaultAllocator::Alloc(size, static_cast<size_t>(align));
//     if (!ptr)
//     {
//         throw std::bad_alloc();
//     }
//     return ptr;
// }
//
// void operator delete(void* ptr) noexcept
// {
//     if (ptr)
//     {
//         DefaultAllocator::Free(ptr);
//     }
// }
//
// void operator delete(void* ptr, std::align_val_t) noexcept
// {
//     if (ptr)
//     {
//         DefaultAllocator::Free(ptr);
//     }
// }

void* ZEN_CDECL operator new(size_t s, const char* pFileName, uint32_t lineNum) noexcept
{
    return zen::DefaultAllocator::Calloc(s, 1, pFileName, lineNum);
}

// void ZEN_CDECL operator delete(void* pMem)
// {
//     if (pMem)
//     {
//         zen::DefaultAllocator::Free(pMem);
//     }
// }

void* operator new[](size_t s, const char* pFileName, uint32_t lineNum) noexcept
{
    return zen::DefaultAllocator::Calloc(s, 1, pFileName, lineNum);
}

void operator delete(void* pMem, const char* pFileName, uint32_t lineNum) noexcept
{
    if (pMem)
    {
        zen::DefaultAllocator::Free(pMem, pFileName, lineNum);
    }
}
void operator delete[](void* pMem, const char* pFileName, uint32_t lineNum) noexcept
{
    if (pMem)
    {
        zen::DefaultAllocator::Free(pMem, pFileName, lineNum);
    }
}
