#include "Memory/Memory.h"
#include "Utils/Errors.h"

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

void DefaultAllocator::TrackMemAlloc(size_t s)
{
    DefaultAllocator* pAllocator = GetInstance();
    pAllocator->m_totalAllocated += s;
    pAllocator->m_currentUsage += s;

    size_t peak = pAllocator->m_peakUsage.load();
    while (pAllocator->m_currentUsage > peak &&
           !pAllocator->m_peakUsage.compare_exchange_weak(peak, pAllocator->m_currentUsage))
    {
    }
}

void DefaultAllocator::TrackMemReAlloc(size_t oldSize, size_t newSize)
{
    DefaultAllocator* pAllocator = GetInstance();

    if (newSize > oldSize)
    {
        pAllocator->m_totalAllocated += (newSize - oldSize);
        pAllocator->m_currentUsage += (newSize - oldSize);
    }
    else
    {
        pAllocator->m_totalFreed += (oldSize - newSize);
        pAllocator->m_currentUsage -= (oldSize - newSize);
    }

    size_t peak = pAllocator->m_peakUsage.load();
    while (pAllocator->m_currentUsage > peak &&
           !pAllocator->m_peakUsage.compare_exchange_weak(peak, pAllocator->m_currentUsage))
    {
    }
}

void DefaultAllocator::TrackMemFree(size_t s)
{
    DefaultAllocator* pAllocator = GetInstance();
    pAllocator->m_totalFreed += s;
    pAllocator->m_currentUsage -= s;
}

void* DefaultAllocator::Alloc(size_t s, size_t alignment, const char* pFileName, uint32_t lineNum)
{
#if defined(ZEN_DEBUG)
    const size_t totalSize = sizeof(AllocationHeader) + s;
    AllocationHeader* header =
        static_cast<AllocationHeader*>(DefaultAllocImpl(totalSize, alignment));

    ASSERT(header != nullptr);

    header->size_      = s;
    header->pFileName  = pFileName;
    header->lineNumber = lineNum;

    TrackMemAlloc(s);

    return header + 1;
#else
    void* pMemory = DefaultAllocImpl(size, alignment);
    return pMemory;
#endif
}

void* DefaultAllocator::Calloc(size_t s, size_t alignment, const char* pFileName, uint32_t lineNumm)
{
    void* p = Alloc(s, alignment, pFileName, lineNumm);
    std::memset(p, 0, s);
    return p;
}

void DefaultAllocator::Free(void* pMemory, const char* pFileName, uint32_t lineNumm)
{
    if (!pMemory)
        return;

#if defined(ZEN_DEBUG)
    auto* header = static_cast<AllocationHeader*>(pMemory) - 1;

    TrackMemFree(header->size_);

    DefaultFreeImpl(header);
#else
    DefaultFreeImpl(pMemory);
#endif
}

void* DefaultAllocator::Realloc(void* ptr,
                                size_t newSize,
                                size_t alignment,
                                const char* pFileName,
                                uint32_t lineNumm)
{
#if defined(ZEN_DEBUG)
    if (!ptr)
        return Alloc(newSize, alignment, pFileName, lineNumm);

    auto* oldHeader = reinterpret_cast<AllocationHeader*>(ptr) - 1;
    size_t oldSize  = oldHeader->size_;

    const size_t totalSize = sizeof(AllocationHeader) + newSize;
    void* raw              = DefaultReallocImpl(oldHeader, totalSize, alignment);
    assert(raw);

    auto* newHeader  = reinterpret_cast<AllocationHeader*>(raw);
    newHeader->size_ = newSize;

    TrackMemReAlloc(oldSize, newSize);

    return newHeader + 1;
#else
    return DefaultReallocImpl(ptr, newSize, alignment);
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

static void PrintMemoryLine(const char* label, size_t bytes)
{
    printf("%-16s : ", label);
    PrintMemorySize(bytes);
    printf("\n");
}

void DefaultAllocator::ReportMemUsage()
{
    DefaultAllocator* alloc = GetInstance();

    const size_t totalAllocated = alloc->m_totalAllocated.load();
    const size_t totalFreed     = alloc->m_totalFreed.load();
    const size_t currentUsage   = alloc->m_currentUsage.load();
    const size_t peakUsage      = alloc->m_peakUsage.load();

    printf("\n========== DefaultAllocator Memory Report ==========\n");

    PrintMemoryLine("Total Allocated", totalAllocated);
    PrintMemoryLine("Total Freed", totalFreed);
    PrintMemoryLine("Current Usage", currentUsage);
    PrintMemoryLine("Peak Usage", peakUsage);

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

void* DefaultAllocator::DefaultReallocImpl(void* ptr, size_t size, size_t alignment)
{
    // nullptr do alloc
    if (ptr == nullptr)
    {
        return DefaultAllocImpl(size, alignment);
    }

    void* pNewMem;
#if _POSIX_VERSION >= 200112L || defined(ZEN_MACOS)
    // posix_memalign does not support realloc, so we need to manually handle it
    pNewMem = DefaultAllocImpl(size, alignment);
    if (pNewMem)
    {
        // Assuming we know the old size, for simplicity let's use memcpy
        std::memcpy(pNewMem, ptr, size);
        DefaultFreeImpl(ptr);
    }
#elif defined(_MSC_VER)
    pNewMem = _aligned_realloc(ptr, size, Pow2Pad(alignment));
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