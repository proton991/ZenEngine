#include "Graphics/VulkanRHI/VulkanMemory.h"
#include "Graphics/RHI/RHICommon.h"
#include "Graphics/RHI/RHIOptions.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"

namespace zen
{
static constexpr uint32_t SMALL_VK_ALLOCATION_SIZE = 4096;

// Tightly packed texels bound an image's native memory requirement from below.
// Formats without a known texel size report zero.
static uint64_t GetPackedImageBytes(const VkImageCreateInfo& imageCI)
{
    const uint64_t texelBytes = GetTextureFormatPixelSize(static_cast<DataFormat>(imageCI.format));

    uint64_t texels = 0;

    uint64_t width = imageCI.extent.width;

    uint64_t height = imageCI.extent.height;

    uint64_t depth = imageCI.extent.depth;

    for (uint32_t mip = 0; mip < imageCI.mipLevels; ++mip)
    {
        texels += width * height * depth;

        width = std::max<uint64_t>(1, width / 2);

        height = std::max<uint64_t>(1, height / 2);

        depth = std::max<uint64_t>(1, depth / 2);
    }

    return texels * texelBytes * imageCI.arrayLayers * uint64_t(imageCI.samples);
}

static void AddMemory(std::atomic<uint64_t>& live, std::atomic<uint64_t>& peak, uint64_t bytes)
{
    const uint64_t current = live.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    uint64_t previous      = peak.load(std::memory_order_relaxed);
    while (previous < current &&
           !peak.compare_exchange_weak(previous, current, std::memory_order_relaxed))
    {
    }
}

void VKAPI_PTR VulkanMemoryAllocator::MemoryAllocated(VmaAllocator,
                                                      uint32_t memoryType,
                                                      VkDeviceMemory,
                                                      VkDeviceSize size,
                                                      void* userData)
{
    static_cast<VulkanMemoryAllocator*>(userData)->TrackMemory(memoryType, size, true);
}

void VKAPI_PTR VulkanMemoryAllocator::MemoryFreed(VmaAllocator,
                                                  uint32_t memoryType,
                                                  VkDeviceMemory,
                                                  VkDeviceSize size,
                                                  void* userData)
{
    static_cast<VulkanMemoryAllocator*>(userData)->TrackMemory(memoryType, size, false);
}

void VulkanMemoryAllocator::TrackMemory(uint32_t memoryType, VkDeviceSize size, bool allocated)
{
    const bool deviceLocal = (m_memoryProperties.memoryTypes[memoryType].propertyFlags &
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
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

        // destroy pools
        for (std::pair<const uint32_t, VmaPool_T*>& kv : m_smallPools)
        {
            vmaDestroyPool(m_vmaAllocator, kv.second);
        }

        vmaDestroyAllocator(m_vmaAllocator);
        if (m_logMemoryStats)
        {
            LOGI(
                "GPU memory VMA: peak_committed_bytes={} peak_device_local_bytes={} remaining_bytes={}",
                m_peakBytes.load(), m_peakDeviceBytes.load(), m_liveBytes.load());
        }
    }
}

void VulkanMemoryAllocator::Init(VkInstance instance,
                                 VkPhysicalDevice gpu,
                                 VkDevice device,
                                 bool bufferDeviceAddress)
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
    allocatorCI.pVulkanFunctions = &vmaVkFunc;
    m_logMemoryStats             = RHIOptions::GetInstance().GPUMemoryStats();

    // Block allocation/free callbacks are inexpensive and keep the live UI available
    // without enabling the optional shutdown log or scanning allocations every frame.
    VmaDeviceMemoryCallbacks memoryCallbacks{MemoryAllocated, MemoryFreed, this};

    vkGetPhysicalDeviceMemoryProperties(gpu, &m_memoryProperties);

    m_device = device;

    allocatorCI.pDeviceMemoryCallbacks = &memoryCallbacks;

    VKCHECK(vmaCreateAllocator(&allocatorCI, &m_vmaAllocator));
}

RHIGPUMemoryStats VulkanMemoryAllocator::GetGPUMemoryStats() const
{
    RHIGPUMemoryStats stats;

    stats.available = m_vmaAllocator != VK_NULL_HANDLE;

    stats.committedBytes = m_liveBytes.load(std::memory_order_relaxed);

    stats.deviceLocalBytes = m_liveDeviceBytes.load(std::memory_order_relaxed);

    // Allocation callbacks update live before peak; avoid displaying a smaller peak
    // if this sample lands between those atomic updates.
    stats.peakCommittedBytes =
        std::max(stats.committedBytes, m_peakBytes.load(std::memory_order_relaxed));

    stats.peakDeviceLocalBytes =
        std::max(stats.deviceLocalBytes, m_peakDeviceBytes.load(std::memory_order_relaxed));

    return stats;
}

RHIGPUMemoryStats VulkanRHI::GetGPUMemoryStats() const
{
    return GVkMemAllocator != nullptr ? GVkMemAllocator->GetGPUMemoryStats() : RHIGPUMemoryStats{};
}

bool VulkanMemoryAllocator::AllocImage(const VkImageCreateInfo* pImageCI,
                                       bool cpuReadable,
                                       VkImage* pImage,
                                       VulkanMemoryAllocation* pAllocation)
{
    VmaAllocationCreateInfo vmaAllocationCI{};
    vmaAllocationCI.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    vmaAllocationCI.flags = cpuReadable ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT : 0;
    *pImage               = VK_NULL_HANDLE;
    *pAllocation          = {};
    bool ready            = true;

    // Small images share a pool, but a failed pool must never be published or used.
    if (IsSmallImage(*pImageCI))
    {
        uint32_t memTypeIndex = 0;
        const VkResult result = vmaFindMemoryTypeIndexForImageInfo(m_vmaAllocator, pImageCI,
                                                                   &vmaAllocationCI, &memTypeIndex);
        ready                 = result == VK_SUCCESS;
        if (ready)
        {
            vmaAllocationCI.pool = GetOrCreateSmallAllocPools(memTypeIndex);
            ready                = vmaAllocationCI.pool != VK_NULL_HANDLE;
        }
        else
        {
            LOGE("Image memory type selection failed: {}", GetResultString(result));
        }
    }

    if (ready)
    {
        const VkResult result = vmaCreateImage(m_vmaAllocator, pImageCI, &vmaAllocationCI, pImage,
                                               &pAllocation->handle, &pAllocation->info);
        ready                 = result == VK_SUCCESS;
        if (!ready)
        {
            LOGE("Image allocation failed: {}", GetResultString(result));
            *pImage      = VK_NULL_HANDLE;
            *pAllocation = {};
        }
    }
    return ready;
}

void VulkanMemoryAllocator::FreeImage(VkImage image, const VulkanMemoryAllocation& memAlloc)
{
    vmaDestroyImage(m_vmaAllocator, image, memAlloc.handle);
}

void VulkanMemoryAllocator::AllocBuffer(uint64_t size,
                                        const VkBufferCreateInfo* pBufferCI,
                                        RHIBufferAllocateType allocType,
                                        VkBuffer* pBuffer,
                                        VulkanMemoryAllocation* pAllocation)
{
    VmaAllocationCreateInfo vmaAllocationCI{};

    if (allocType == RHIBufferAllocateType::eCPURead ||
        allocType == RHIBufferAllocateType::eCPUWrite ||
        allocType == RHIBufferAllocateType::eCPUWriteGPURead)
    {
        vmaAllocationCI.flags = allocType != RHIBufferAllocateType::eCPURead ?
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT :
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
        vmaAllocationCI.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT;

        vmaAllocationCI.usage = allocType == RHIBufferAllocateType::eCPUWriteGPURead ?
            VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE :
            VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        vmaAllocationCI.requiredFlags =
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    }
    else if (allocType == RHIBufferAllocateType::eGPU)
    {
        vmaAllocationCI.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

        if (size <= SMALL_VK_ALLOCATION_SIZE)
        {
            uint32_t memTypeIndex = 0;
            vmaFindMemoryTypeIndexForBufferInfo(m_vmaAllocator, pBufferCI, &vmaAllocationCI,
                                                &memTypeIndex);
            vmaAllocationCI.pool = GetOrCreateSmallAllocPools(memTypeIndex);
        }
    }

    VKCHECK(vmaCreateBuffer(m_vmaAllocator, pBufferCI, &vmaAllocationCI, pBuffer,
                            &pAllocation->handle, &pAllocation->info));
}

uint8_t* VulkanMemoryAllocator::MapBuffer(const VulkanMemoryAllocation& memAlloc)
{
    void* pDataPtr = nullptr;
    VKCHECK(vmaMapMemory(m_vmaAllocator, memAlloc.handle, &pDataPtr));

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

VmaPool VulkanMemoryAllocator::GetOrCreateSmallAllocPools(MemoryTypeIndex memTypeIndex)
{
    VmaPool result{};

    if (m_smallPools.contains(memTypeIndex))
    {
        result = m_smallPools[memTypeIndex];
    }
    else
    {
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
        const VkResult status = vmaCreatePool(m_vmaAllocator, &poolCI, &pool);
        if (status == VK_SUCCESS)
        {
            m_smallPools[memTypeIndex] = pool;
            result                     = pool;
        }
        else
        {
            LOGE("Small allocation pool creation failed: {}", GetResultString(status));
        }
    }

    return result;
}

// Only images whose packed texels fit the small pool pay for a probe of the native requirement.
// A failed probe leaves placement to the default pools, where the real allocation reports it.
bool VulkanMemoryAllocator::IsSmallImage(const VkImageCreateInfo& imageCI) const
{
    bool smallImage = false;

    if (GetPackedImageBytes(imageCI) <= SMALL_VK_ALLOCATION_SIZE)
    {
        VkImage probe{VK_NULL_HANDLE};

        if (vkCreateImage(m_device, &imageCI, nullptr, &probe) == VK_SUCCESS)
        {
            VkMemoryRequirements requirements{};

            vkGetImageMemoryRequirements(m_device, probe, &requirements);

            vkDestroyImage(m_device, probe, nullptr);

            smallImage = requirements.size <= SMALL_VK_ALLOCATION_SIZE;
        }
    }

    return smallImage;
}
} // namespace zen
