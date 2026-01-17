#include "Memory/Memory.h"
#include "Utils/Errors.h"

namespace zen
{
#if defined(ZEN_DEBUG)
std::atomic<size_t> DefaultAllocator::s_TotalAllocated{0};
std::atomic<size_t> DefaultAllocator::s_TotalFreed{0};
std::atomic<size_t> DefaultAllocator::s_CurrentUsage{0};
std::atomic<size_t> DefaultAllocator::s_PeakUsage{0};
#endif

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

    s_TotalAllocated += s;
    s_CurrentUsage += s;

    size_t peak = s_PeakUsage.load();
    while (s_CurrentUsage > peak && !s_PeakUsage.compare_exchange_weak(peak, s_CurrentUsage))
    {
    }

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
    size_t size  = header->size_;

    s_TotalFreed += size;
    s_CurrentUsage -= size;

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

    if (newSize > oldSize)
    {
        s_TotalAllocated += (newSize - oldSize);
        s_CurrentUsage += (newSize - oldSize);
    }
    else
    {
        s_TotalFreed += (oldSize - newSize);
        s_CurrentUsage -= (oldSize - newSize);
    }

    size_t peak = s_PeakUsage.load();
    while (s_CurrentUsage > peak && !s_PeakUsage.compare_exchange_weak(peak, s_CurrentUsage))
    {
    }

    return newHeader + 1;
#else
    return DefaultReallocImpl(ptr, newSize, alignment);
#endif
}

#if defined(ZEN_DEBUG)
void DefaultAllocator::ReportMemUsage()
{
    LOGI("========== DefaultAllocator Memory Report ==========");
    LOGI("Total Allocated : {} bytes", s_TotalAllocated.load());
    LOGI("Total Freed     : {} bytes", s_TotalFreed.load());
    LOGI("Current Usage   : {} bytes", s_CurrentUsage.load());
    LOGI("Peak Usage      : {} bytes", s_PeakUsage.load());

    if (s_CurrentUsage != 0)
    {
        LOGE("MEMORY LEAK DETECTED ({} bytes)", s_CurrentUsage.load());
    }
    else
    {
        LOGI("No memory leaks detected");
    }
    LOGI("====================================================");
}
#endif

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