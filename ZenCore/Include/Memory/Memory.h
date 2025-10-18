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
    inline static void* Alloc(size_t size, size_t alignment = ZEN_DEFAULT_ALIGNMENT)
    {
        void* pMemory = DefaultAllocImpl(size, alignment);
        return pMemory;
    }

    inline static void* Calloc(size_t size, size_t alignment = ZEN_DEFAULT_ALIGNMENT)
    {
        void* pMemory = DefaultCallocImpl(size, alignment);
        return pMemory;
    }

    inline static void Free(void* pMemory)
    {
        DefaultFreeImpl(pMemory);
    }

    inline static void* Realloc(void* ptr, size_t newSize, size_t alignment = ZEN_DEFAULT_ALIGNMENT)
    {
        return DefaultReallocImpl(ptr, newSize, alignment);
    }

private:
    static void* DefaultAllocImpl(size_t size, size_t alignment);

    static void* DefaultCallocImpl(size_t size, size_t alignment);

    static void* DefaultReallocImpl(void* ptr, size_t size, size_t alignment);

    static void DefaultFreeImpl(void* pMem);
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

#define MEM_ALLOC(m_size)          DefaultAllocator::Alloc(m_size)
#define MEM_ALLOC_ZEROED(m_size)   DefaultAllocator::Calloc(m_size)
#define MEM_REALLOC(m_mem, m_size) DefaultAllocator::Realloc(m_mem, m_size)
#define MEM_FREE(m_mem)            DefaultAllocator::Free(m_mem)
} // namespace zen