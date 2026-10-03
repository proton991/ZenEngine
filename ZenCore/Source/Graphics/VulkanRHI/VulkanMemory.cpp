#include "Graphics/VulkanRHI/VulkanMemory.h"
#include "Graphics/RHI/RHICommon.h"
#include "Graphics/RHI/RHIOptions.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"

namespace zen
{
static void AddMemory(std::atomic<uint64_t>& live, std::atomic<uint64_t>& peak, uint64_t bytes)
{
    const uint64_t current = live.fetch_add(bytes, std::memory_order_relaxed) + bytes;

    uint64_t previous      = peak.load(std::memory_order_relaxed);

    while (previous < current && !peak.compare_exchange_weak(previous, current, std::memory_order_relaxed))
    {
    }
}

void VKAPI_PTR
VulkanMemoryAllocator::MemoryAllocated(VmaAllocator, uint32_t memoryType, VkDeviceMemory, VkDeviceSize size, void* userData)
{
    static_cast<VulkanMemoryAllocator*>(userData)->TrackMemory(memoryType, size, true);
}

void VKAPI_PTR
VulkanMemoryAllocator::MemoryFreed(VmaAllocator, uint32_t memoryType, VkDeviceMemory, VkDeviceSize size, void* userData)
{
    static_cast<VulkanMemoryAllocator*>(userData)->TrackMemory(memoryType, size, false);
}

void VulkanMemoryAllocator::TrackMemory(uint32_t memoryType, VkDeviceSize size, bool allocated)
{
    const bool deviceLocal =
        (m_memoryProperties.memoryTypes[memoryType].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;

    if (allocated)
    {
        AddMemory(m_liveBytes, m_peakBytes, size);

        if (deviceLocal)
        {
            AddMemory(m_liveDeviceBytes, m_peakDeviceBytes, size);
        }
    }
    else
    {
        m_liveBytes.fetch_sub(size, std::memory_order_relaxed);

        if (deviceLocal)
        {
            m_liveDeviceBytes.fetch_sub(size, std::memory_order_relaxed);
        }
    }
}

VulkanMemoryAllocator::~VulkanMemoryAllocator()
{
    if (m_vmaAllocator != VK_NULL_HANDLE)
    {
        VmaTotalStatistics stats;

        vmaCalculateStatistics(m_vmaAllocator, &stats);

        LOGI("VMA Total device memory leaked: {} bytes.", stats.total.statistics.allocationBytes);

        vmaDestroyAllocator(m_vmaAllocator);

        if (m_logMemoryStats)
        {
            LOGI("GPU memory VMA: peak_committed_bytes={} peak_device_local_bytes={} remaining_bytes={}", m_peakBytes.load(),
                 m_peakDeviceBytes.load(), m_liveBytes.load());
        }
    }
}

void VulkanMemoryAllocator::Init(VkInstance       instance,
                                 VkPhysicalDevice gpu,
                                 VkDevice         device,
                                 bool             bufferDeviceAddress,
                                 bool             memoryBudget)
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

    allocatorCI.instance         = instance;

    allocatorCI.device           = device;

    allocatorCI.physicalDevice   = gpu;

    allocatorCI.vulkanApiVersion = VK_API_VERSION_1_2;

    if (bufferDeviceAddress)
    {
        allocatorCI.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    }

    m_memoryBudget = memoryBudget;

    if (m_memoryBudget)
    {
        allocatorCI.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
    }

    allocatorCI.pVulkanFunctions = &vmaVkFunc;

    m_logMemoryStats             = RHIOptions::GetInstance().GPUMemoryStats();

    // Block allocation/free callbacks are inexpensive and keep the live UI available
    // without enabling the optional shutdown log or scanning allocations every frame.
    VmaDeviceMemoryCallbacks memoryCallbacks{MemoryAllocated, MemoryFreed, this};

    vkGetPhysicalDeviceMemoryProperties(gpu, &m_memoryProperties);

    allocatorCI.pDeviceMemoryCallbacks = &memoryCallbacks;

    VERIFY_EXPR_MSG_F((vmaCreateAllocator(&allocatorCI, &m_vmaAllocator)) == VK_SUCCESS,
                      "Vulkan operation failed: vmaCreateAllocator(&allocatorCI, &m_vmaAllocator)");
}

void VulkanMemoryAllocator::BeginFrame(uint32_t frame)
{
    vmaSetCurrentFrameIndex(m_vmaAllocator, frame);
}

RHIGPUMemoryStats VulkanMemoryAllocator::GetGPUMemoryStats() const
{
    RHIGPUMemoryStats stats;

    stats.available        = m_vmaAllocator != VK_NULL_HANDLE;

    stats.committedBytes   = m_liveBytes.load(std::memory_order_relaxed);

    stats.deviceLocalBytes = m_liveDeviceBytes.load(std::memory_order_relaxed);

    // Allocation callbacks update live before peak; avoid displaying a smaller peak
    // if this sample lands between those atomic updates.
    stats.peakCommittedBytes   = std::max(stats.committedBytes, m_peakBytes.load(std::memory_order_relaxed));

    stats.peakDeviceLocalBytes = std::max(stats.deviceLocalBytes, m_peakDeviceBytes.load(std::memory_order_relaxed));

    if (stats.available && m_memoryBudget)
    {
        VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};

        vmaGetHeapBudgets(m_vmaAllocator, budgets);

        stats.budgetAvailable = true;

        stats.heapCount       = m_memoryProperties.memoryHeapCount;

        for (uint32_t i = 0; i < stats.heapCount; ++i)
        {
            stats.heaps[i] = {m_memoryProperties.memoryHeaps[i].size, budgets[i].usage, budgets[i].budget,
                              (m_memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0};
        }
    }

    return stats;
}

RHIGPUMemoryStats VulkanRHI::GetGPUMemoryStats() const
{
    return GVkMemAllocator != nullptr ? GVkMemAllocator->GetGPUMemoryStats() : RHIGPUMemoryStats{};
}

bool VulkanMemoryAllocator::AllocImage(const VkImageCreateInfo* pImageCI,
                                       bool                     cpuReadable,
                                       VkImage*                 pImage,
                                       VulkanMemoryAllocation*  pAllocation)
{
    // VMA's default pools sub-allocate small images and buffers from shared blocks.
    VmaAllocationCreateInfo vmaAllocationCI{};

    vmaAllocationCI.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    vmaAllocationCI.flags = cpuReadable ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT : 0;

    if (m_memoryBudget)
    {
        vmaAllocationCI.flags |= VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
    }

    *pImage      = VK_NULL_HANDLE;

    *pAllocation = {};

    const VkResult result =
        vmaCreateImage(m_vmaAllocator, pImageCI, &vmaAllocationCI, pImage, &pAllocation->handle, &pAllocation->info);

    const bool ready = result == VK_SUCCESS;

    if (!ready)
    {
        ReportVulkanDeviceLoss(result, "vmaCreateImage");

        LOGE("Image allocation failed: {}", GetResultString(result));

        *pImage      = VK_NULL_HANDLE;

        *pAllocation = {};
    }

    return ready;
}

void VulkanMemoryAllocator::FreeImage(VkImage image, const VulkanMemoryAllocation& memAlloc)
{
    vmaDestroyImage(m_vmaAllocator, image, memAlloc.handle);
}

bool VulkanMemoryAllocator::AllocBuffer(uint64_t                  size,
                                        const VkBufferCreateInfo* pBufferCI,
                                        RHIBufferAllocateType     allocType,
                                        VkBuffer*                 pBuffer,
                                        VulkanMemoryAllocation*   pAllocation)
{
    VmaAllocationCreateInfo vmaAllocationCI{};

    if (allocType == RHIBufferAllocateType::eCPURead || allocType == RHIBufferAllocateType::eCPUWrite
        || allocType == RHIBufferAllocateType::eCPUWriteGPURead)
    {
        vmaAllocationCI.flags  = allocType != RHIBufferAllocateType::eCPURead
                                   ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                                   : VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;

        vmaAllocationCI.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT;

        vmaAllocationCI.usage  = allocType == RHIBufferAllocateType::eCPUWriteGPURead ? VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE
                                                                                      : VMA_MEMORY_USAGE_AUTO_PREFER_HOST;

        vmaAllocationCI.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    }
    else if (allocType == RHIBufferAllocateType::eGPU)
    {
        vmaAllocationCI.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    }

    if (m_memoryBudget)
    {
        vmaAllocationCI.flags |= VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
    }

    *pBuffer     = VK_NULL_HANDLE;

    *pAllocation = {};

    const VkResult result =
        vmaCreateBuffer(m_vmaAllocator, pBufferCI, &vmaAllocationCI, pBuffer, &pAllocation->handle, &pAllocation->info);

    const bool mappedRequired = (vmaAllocationCI.flags & VMA_ALLOCATION_CREATE_MAPPED_BIT) != 0;

    const bool ready          = result == VK_SUCCESS && *pBuffer != VK_NULL_HANDLE && pAllocation->handle != VK_NULL_HANDLE
                    && (!mappedRequired || pAllocation->info.pMappedData != nullptr);

    if (!ready)
    {
        ReportVulkanDeviceLoss(result, "vmaCreateBuffer");

        LOGE("Buffer allocation or persistent mapping failed: {}", GetResultString(result));

        if (*pBuffer != VK_NULL_HANDLE || pAllocation->handle != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(m_vmaAllocator, *pBuffer, pAllocation->handle);
        }

        *pBuffer     = VK_NULL_HANDLE;

        *pAllocation = {};
    }

    return ready;
}

uint8_t* VulkanMemoryAllocator::MapBuffer(const VulkanMemoryAllocation& memAlloc)
{
    void* pDataPtr        = nullptr;

    const VkResult result = memAlloc.handle != VK_NULL_HANDLE ? vmaMapMemory(m_vmaAllocator, memAlloc.handle, &pDataPtr)
                                                              : VK_ERROR_MEMORY_MAP_FAILED;

    if (result != VK_SUCCESS)
    {
        ReportVulkanDeviceLoss(result, "vmaMapMemory");

        LOGE("Buffer mapping failed: {}", GetResultString(result));

        pDataPtr = nullptr;
    }

    return static_cast<uint8_t*>(pDataPtr);
}

void VulkanMemoryAllocator::UnmapBuffer(const VulkanMemoryAllocation& memAlloc)
{
    vmaUnmapMemory(m_vmaAllocator, memAlloc.handle);
}

void VulkanMemoryAllocator::FreeBuffer(VkBuffer buffer, const VulkanMemoryAllocation& memAlloc)
{
    vmaDestroyBuffer(m_vmaAllocator, buffer, memAlloc.handle);
}
} // namespace zen
