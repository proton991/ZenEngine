#pragma once
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>

#define ZEN_DEFAULT_ALIGNMENT 16

#define PRIVATE_ARRAY_DEF(Type, Name, BasePtr)          \
    Type* Name()                                        \
    {                                                   \
        return reinterpret_cast<Type*>(BasePtr);        \
    }                                                   \
                                                        \
    Type* const* Name() const                           \
    {                                                   \
        return reinterpret_cast<Type* const*>(BasePtr); \
    }

#if defined(ZEN_WIN32)
#    define ZEN_CDECL __cdecl
#else
#    define ZEN_CDECL
#endif

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
    static void* Alloc(size_t s, size_t alignment, const char* pFileName, uint32_t lineNum);

    static void* Calloc(size_t s, size_t alignment, const char* pFileName, uint32_t lineNumm);

    static void Free(void* pMemory, const char* pFileName, uint32_t lineNumm);

    static void* Realloc(void* ptr,
                         size_t newSize,
                         size_t alignment,
                         const char* pFileName,
                         uint32_t lineNumm);

#if defined(ZEN_DEBUG)
    static void ReportMemUsage();
#endif

private:
    static void* DefaultAllocImpl(size_t size, size_t alignment);

    static void* DefaultCallocImpl(size_t size, size_t alignment);

    static void* DefaultReallocImpl(void* ptr, size_t size, size_t alignment);

    static void DefaultFreeImpl(void* pMem);

#if defined(ZEN_DEBUG)
    struct AllocationHeader
    {
        size_t size_;
        const char* pFileName;
        uint32_t lineNumber;
    };

    static std::atomic<size_t> s_TotalAllocated;
    static std::atomic<size_t> s_TotalFreed;
    static std::atomic<size_t> s_CurrentUsage;
    static std::atomic<size_t> s_PeakUsage;
#endif
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


void* ZEN_CDECL operator new(size_t s, const char* pFileName, uint32_t lineNum) noexcept;

void* ZEN_CDECL operator new[](size_t s, const char* pFileName, uint32_t lineNum) noexcept;

void ZEN_CDECL operator delete(void* pMem, const char* pFileName, uint32_t lineNum) noexcept;

void ZEN_CDECL operator delete[](void* pMem, const char* pFileName, uint32_t lineNum) noexcept;

template <class T> inline void ZenDelete(T* obj, const char* pFileName, uint32_t lineNum)
{
    if (obj != nullptr)
    {
        obj->~T();
        zen::DefaultAllocator::Free(obj, pFileName, lineNum);
    }
}

// aligned/not-aligned
#define ZEN_MEM_ALLOC(SIZE)        zen::DefaultAllocator::Alloc(SIZE, 1, __FILE__, __LINE__)
#define ZEN_MEM_CALLOC(SIZE)       zen::DefaultAllocator::Calloc(SIZE, 1, __FILE__, __LINE__)
#define ZEN_MEM_REALLOC(MEM, SIZE) zen::DefaultAllocator::Realloc(MEM, SIZE, 1, __FILE__, __LINE__)
#define ZEN_MEM_FREE(MEM)          zen::DefaultAllocator::Free(MEM, __FILE__, __LINE__)

#define ZEN_MEM_ALLOC_ALIGNED(SIZE, ALIGN) \
    zen::DefaultAllocator::Alloc(SIZE, ALIGN, __FILE__, __LINE__)
#define ZEN_MEM_CALLOC_ALIGNED(SIZE, ALIGN) \
    zen::DefaultAllocator::Calloc(SIZE, ALIGN, __FILE__, __LINE__)
#define ZEN_MEM_REALLOC_ALIGNED(MEM, SIZE, ALIGN) \
    zen::DefaultAllocator::Realloc(MEM, SIZE, ALIGN, __FILE__, __LINE__)

#define ZEN_NEW() new (__FILE__, __LINE__)

#define ZEN_DELETE(PTR) ZenDelete(PTR, __FILE__, __LINE__)