#pragma once
#include "VulkanBuffer.h"
#include "VulkanTexture.h"
#include "VulkanPipeline.h"
#include "Common/PagedAllocator.h"

namespace zen::rhi
{
// This helps using a single paged allocator for many resource types.
template <typename... RESOURCE_TYPES> struct VersatileResourceTemplate
{
    static constexpr size_t RESOURCE_SIZES[] = {sizeof(RESOURCE_TYPES)...};
    static constexpr size_t MAX_RESOURCE_SIZE =
        std::max_element(RESOURCE_SIZES, RESOURCE_SIZES + sizeof...(RESOURCE_TYPES))[0];
    uint8_t data[MAX_RESOURCE_SIZE];

    template <typename T> static T* Alloc(PagedAllocator<VersatileResourceTemplate>& allocator)
    {
        T* obj = (T*)allocator.Alloc();
        new (obj) T;
        return obj;
    }

    template <typename T>
    static void Free(PagedAllocator<VersatileResourceTemplate>& p_allocator, T* p_object)
    {
        p_object->~T();
        p_allocator.Free((VersatileResourceTemplate*)p_object);
    }
};
} // namespace zen::rhi