#include "Graphics/VulkanRHI/VulkanMemory.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"



namespace zen::rhi
{
static constexpr uint32_t SMALL_VK_ALLOCATION_SIZE = 4096;

VulkanMemoryAllocator::~VulkanMemoryAllocator()
{
    if (m_vmaAllocator != VK_NULL_HANDLE)
    {
        VmaTotalStatistics stats;
        vmaCalculateStatistics(m_vmaAllocator, &stats);

        LOGI("VMA Total device memory leaked: {} bytes.", stats.total.statistics.allocationBytes);

        // destroy pools
        for (auto& kv : m_smallPools)
        {
            vmaDestroyPool(m_vmaAllocator, kv.second);
        }
        vmaDestroyAllocator(m_vmaAllocator);
    }
}

void VulkanMemoryAllocator::Init(VkInstance instance, VkPhysicalDevice gpu, VkDevice device)
{
    // pass dynamic function pointers to vma
    VmaVulkanFunctions vmaVkFunc{};
    vmaVkFunc.vkGetInstanceProcAddr               = vkGetInstanceProcAddr;
    vmaVkFunc.vkGetDeviceProcAddr                 = vkGetDeviceProcAddr;
    vmaVkFunc.vkAllocateMemory                    = vkAllocateMemory;
    vmaVkFunc.vkBindBufferMemory                  = vkBindBufferMemory;
    vmaVkFunc.vkBindImageMemory                   = vkBindImageMemory;
    vmaVkFunc.vkCreateBuffer                      = vkCreateBuffer;
    vmaVkFunc.vkCreateImage                       = vkCreateImage;
    vmaVkFunc.vkDestroyBuffer                     = vkDestroyBuffer;
    vmaVkFunc.vkDestroyImage                      = vkDestroyImage;
    vmaVkFunc.vkFlushMappedMemoryRanges           = vkFlushMappedMemoryRanges;
    vmaVkFunc.vkFreeMemory                        = vkFreeMemory;
    vmaVkFunc.vkGetBufferMemoryRequirements       = vkGetBufferMemoryRequirements;
    vmaVkFunc.vkGetImageMemoryRequirements        = vkGetImageMemoryRequirements;
    vmaVkFunc.vkGetPhysicalDeviceMemoryProperties = vkGetPhysicalDeviceMemoryProperties;
    vmaVkFunc.vkGetPhysicalDeviceProperties       = vkGetPhysicalDeviceProperties;
    vmaVkFunc.vkInvalidateMappedMemoryRanges      = vkInvalidateMappedMemoryRanges;
    vmaVkFunc.vkMapMemory                         = vkMapMemory;
    vmaVkFunc.vkUnmapMemory                       = vkUnmapMemory;
    vmaVkFunc.vkCmdCopyBuffer                     = vkCmdCopyBuffer;

    VmaAllocatorCreateInfo allocatorCI{};
    allocatorCI.instance       = instance;
    allocatorCI.device         = device;
    allocatorCI.physicalDevice = gpu;
    allocatorCI.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    allocatorCI.pVulkanFunctions = &vmaVkFunc;
    VKCHECK(vmaCreateAllocator(&allocatorCI, &m_vmaAllocator));
}

void VulkanMemoryAllocator::AllocImage(const VkImageCreateInfo* imageCI,
                                       bool cpuReadable,
                                       VkImage* image,
                                       VulkanMemoryAllocation* allocation,
                                       uint32_t size)
{

    VmaAllocationCreateInfo vmaAllocationCI{};
    vmaAllocationCI.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    vmaAllocationCI.flags = cpuReadable ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT : 0;
    // use separete pools for samll size memory allocation
    if (size <= SMALL_VK_ALLOCATION_SIZE)
    {
        uint32_t memTypeIndex = 0;
        vmaFindMemoryTypeIndexForImageInfo(m_vmaAllocator, imageCI, &vmaAllocationCI,
                                           &memTypeIndex);
        vmaAllocationCI.pool = GetOrCreateSmallAllocPools(memTypeIndex);
    }

    VKCHECK(vmaCreateImage(m_vmaAllocator, imageCI, &vmaAllocationCI, image, &allocation->handle,
                           &allocation->info));
}

void VulkanMemoryAllocator::FreeImage(VkImage image, const VulkanMemoryAllocation& memAlloc)
{
    vmaDestroyImage(m_vmaAllocator, image, memAlloc.handle);
}

void VulkanMemoryAllocator::AllocBuffer(uint32_t size,
                                        const VkBufferCreateInfo* bufferCI,
                                        BufferAllocateType allocType,
                                        VkBuffer* buffer,
                                        VulkanMemoryAllocation* allocation)
{
    VmaAllocationCreateInfo vmaAllocationCI{};
    if (allocType == BufferAllocateType::eCPU)
    {
        bool isSrc = false;
        bool isDst = false;
        if (bufferCI->usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT)
        {
            isSrc = true;
        }
        if (bufferCI->usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT)
        {
            isDst = true;
        }
        if (isSrc && !isDst)
        {
            // staging buffer: CPU maps, writes sequentially, then GPU copies to VRAM.
            vmaAllocationCI.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        }
        if (!isSrc && isDst)
        {
            // readback buffer: GPU copies from VRAM, then CPU maps and reads.
            vmaAllocationCI.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
        }
        vmaAllocationCI.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        vmaAllocationCI.requiredFlags =
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    }
    else if (allocType == BufferAllocateType::eGPU)
    {
        vmaAllocationCI.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (size <= SMALL_VK_ALLOCATION_SIZE)
        {
            uint32_t memTypeIndex = 0;
            vmaFindMemoryTypeIndexForBufferInfo(m_vmaAllocator, bufferCI, &vmaAllocationCI,
                                                &memTypeIndex);
            vmaAllocationCI.pool = GetOrCreateSmallAllocPools(memTypeIndex);
        }
    }
    VKCHECK(vmaCreateBuffer(m_vmaAllocator, bufferCI, &vmaAllocationCI, buffer, &allocation->handle,
                            &allocation->info));
}

uint8_t* VulkanMemoryAllocator::MapBuffer(const VulkanMemoryAllocation& memAlloc)
{
    void* dataPtr = nullptr;
    VKCHECK(vmaMapMemory(m_vmaAllocator, memAlloc.handle, &dataPtr));
    return static_cast<uint8_t*>(dataPtr);
}

void VulkanMemoryAllocator::UnmapBuffer(const VulkanMemoryAllocation& memAlloc)
{
    vmaUnmapMemory(m_vmaAllocator, memAlloc.handle);
}


void VulkanMemoryAllocator::FreeBuffer(VkBuffer buffer, const VulkanMemoryAllocation& memAlloc)
{
    vmaDestroyBuffer(m_vmaAllocator, buffer, memAlloc.handle);
}

VmaPool VulkanMemoryAllocator::GetOrCreateSmallAllocPools(MemoryTypeIndex memTypeIndex)
{
    if (m_smallPools.contains(memTypeIndex))
    {
        return m_smallPools[memTypeIndex];
    }
    // create a new one
    VmaPoolCreateInfo poolCI{};
    poolCI.memoryTypeIndex        = memTypeIndex;
    poolCI.flags                  = 0;
    poolCI.blockSize              = 0;
    poolCI.minBlockCount          = 0;
    poolCI.maxBlockCount          = SIZE_MAX;
    poolCI.priority               = 0.5f;
    poolCI.minAllocationAlignment = 0;
    poolCI.pMemoryAllocateNext    = nullptr;
    VmaPool pool{VK_NULL_HANDLE};
    VKCHECK(vmaCreatePool(m_vmaAllocator, &poolCI, &pool));
    m_smallPools[memTypeIndex] = pool;

    return pool;
}
} // namespace zen::rhi