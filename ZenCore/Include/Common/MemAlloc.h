#pragma once
#define ZEN_DEFAULT_ALIGNMENT   16
#define ZEN_PLACEMENT_NEW(_ptr) new (_ptr)

#include <malloc.h>
#include <cstdint>

namespace zen::util
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
inline constexpr T Pow2Align(T        value,     ///< Value to align.
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
    if (IsPowerOfTwo(value)) { ret = value; }
    else
    {
        while (ret < value) { ret <<= 1; }
    }

    return ret;
}

void* LinearAlloc(size_t size, size_t alignment)
{
    void* pMem;
#if _POSIX_VERSION >= 20112L
    if (posix_memalign(&pMem, Pow2Align(alginment, sizeof((void*))), size)) { pMem = nullptr; }
#elif _MSC_VER
    pMem = _aligned_malloc(size, Pow2Pad(size));
#else
#    error "Unsuported Platform"
#endif
    return pMem;
}

void* AllocObject(const size_t objectSize)
{
    return LinearAlloc(objectSize, ZEN_DEFAULT_ALIGNMENT);
}

template <typename T> T HandleFromVoidPtr(void* pData)
{
    return T(reinterpret_cast<uint64_t>(pData));
}
} // namespace zen::util