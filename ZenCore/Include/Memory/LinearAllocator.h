#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>  // for std::max_align_t
#include "Memory.h" // your ZEN_MEM_ALLOC, ZEN_MEM_FREE macros

namespace zen
{

class LinearAllocator
{
public:
    LinearAllocator(size_t capacity) : m_capacity(capacity)
    {
        m_buffer = static_cast<uint8_t*>(ZEN_MEM_ALLOC(capacity));
        m_offset = 0;
    }

    // Rule of Zero/Five: Disable copy and move operations    // as this class owns the underlying memory buffer.
    LinearAllocator(const LinearAllocator&)            = delete;
    LinearAllocator& operator=(const LinearAllocator&) = delete;
    LinearAllocator(LinearAllocator&&)                 = delete;
    LinearAllocator& operator=(LinearAllocator&&)      = delete;

    ~LinearAllocator()
    {
        ZEN_MEM_FREE(m_buffer);
        // Set members to zero for safety/clarity (optional, but good practice)
        m_buffer   = nullptr;
        m_capacity = 0;
        m_offset   = 0;
    }

    void* Alloc(size_t size, size_t alignment = alignof(std::max_align_t))
    {
        uintptr_t base    = reinterpret_cast<uintptr_t>(m_buffer);
        uintptr_t cur     = base + m_offset;
        uintptr_t aligned = AlignUp(cur, alignment);

        size_t newOffset = (aligned - base) + size;
        if (newOffset > m_capacity)
            return nullptr; // out of memory

        m_offset = newOffset;
        return reinterpret_cast<void*>(aligned);
    }

    void Reset()
    {
        m_offset = 0;
    }

    size_t Used() const
    {
        return m_offset;
    }
    size_t Capacity() const
    {
        return m_capacity;
    }

private:
    // Helper function to calculate the next aligned address
    static uintptr_t AlignUp(uintptr_t ptr, size_t align)
    {
        return (ptr + (align - 1)) & ~(align - 1);
    }


    uint8_t* m_buffer = nullptr;
    size_t m_capacity = 0;
    size_t m_offset   = 0;
};
} // namespace zen