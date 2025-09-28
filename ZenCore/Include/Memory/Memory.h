#pragma once
#include <cstdlib>
#include <cstring>
#include <new>

#define ZEN_DEFAULT_ALIGNMENT 16
namespace zen
{
/// Determines if a value is a power of two.
///
/// @returns True if it is a power of two, false otherwise.
inline constexpr bool IsPowerOfTwo(uint64_t value)
{
    return value != 0 && ((value & (value - 1)) == 0);
}


/// Rounds the specified uint 'value' up to the nearest value meeting the specified 'alignment'.  Only power of 2
/// alignments are supported by this function.
///
/// returns Aligned value.
template <typename T>
inline constexpr T Pow2Align(T value,            ///< Value to align.
                             uint64_t alignment) ///< Desired alignment (must be a power of 2).
{
    return ((value + static_cast<T>(alignment) - 1) & ~(static_cast<T>(alignment) - 1));
}

/// Rounds the specified uint 'value' up to the nearest power of 2
///
/// @returns Power of 2 padded value.
template <typename T> inline T Pow2Pad(T value) ///< Value to pad.
{
    T ret = 1;
    if (IsPowerOfTwo(value))
    {
        ret = value;
    }
    else
    {
        while (ret < value)
        {
            ret <<= 1;
        }
    }

    return ret;
}


template <typename T> T HandleFromVoidPtr(void* pData)
{
    return T(reinterpret_cast<uint64_t>(pData));
}

class DefaultAllocator
{
public:
    inline static void* Alloc(size_t size)
    {
        void* pMemory = DefaultAllocImpl(size, ZEN_DEFAULT_ALIGNMENT);

        return pMemory;
    }

    inline static void Free(void* pMemory)
    {
        DefaultFreeImpl(pMemory);
    }

    inline static void* Realloc(void* ptr, size_t newSize)
    {
        return DefaultReallocImpl(ptr, newSize, ZEN_DEFAULT_ALIGNMENT);
    }

private:
    inline static void* DefaultAllocImpl(size_t size, size_t alignment)
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
        return pMem;
    }
    inline static void* DefaultReallocImpl(void* ptr, size_t size, size_t alignment)
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

    inline static void DefaultFreeImpl(void* pMem)
    {
#if _POSIX_VERSION >= 20112L || defined(ZEN_MACOS)
        free(pMem);
#elif _MSC_VER
        _aligned_free(pMem);
#else
#    error "Unsuported Platform"
#endif
    }
};

template <typename T, typename... Args> T* MemNew(Args&&... args)
{
    void* pMemory = DefaultAllocator::Alloc(sizeof(T));
    if (!pMemory)
    {
        throw std::bad_alloc();
    }
    return new (pMemory) T(std::forward<Args>(args)...);
}

} // namespace zen