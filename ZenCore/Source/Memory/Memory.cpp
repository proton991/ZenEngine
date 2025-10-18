#include "Memory/Memory.h"
#include "Utils/Errors.h"

// #if defined(ZEN_DEBUG)
// #endif

namespace zen
{
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
