#pragma once
#include "VulkanHeaders.h"
#include "Graphics/RHI/RHICommandList.h"
#include "Utils/UniquePtr.h"
#include "Templates/HeapVector.h"

namespace zen
{
class VulkanRHI;
class VulkanDeviceExtension;
class VulkanQueue;
class LegacyVulkanCommandListContext;
class LegacyVulkanCommandList;
class VulkanFenceManager;
class VulkanSemaphoreManager;

struct DeviceExtensionFlags
{
    uint32_t hasBufferDeviceAddress : 1;
    uint32_t hasAccelerationStructure : 1;
    uint32_t hasRaytracingPipeline : 1;
    uint32_t hasRayQuery : 1;
    uint32_t hasDescriptorIndexing : 1;
    uint32_t hasTimelineSemaphore : 1;

    uint32_t hasDeferredHostOperation : 1;
    uint32_t hasSPIRV_14 : 1;
    uint32_t hasDynamicRendering : 1;
};

class VulkanDevice
{
public:
    explicit VulkanDevice(VkPhysicalDevice gpu);

    void Init();

    void Destroy();

    VkDevice GetVkHandle() const
    {
        return m_device;
    }

    VkPhysicalDeviceProperties GetPhysicalDeviceProperties() const
    {
        return m_gpuProps;
    }

    DeviceExtensionFlags& GetExtensionFlags()
    {
        return m_extensionFlags;
    }

    VkPhysicalDevice GetPhysicalDeviceHandle() const
    {
        return m_gpu;
    }

    void SetObjectName(VkObjectType type, uint64_t handle, const char* pName);

    const auto& GetDescriptorIndexingProperties() const
    {
        return m_descriptorIndexingProperties;
    }

    VulkanFenceManager* GetFenceManager() const
    {
        return m_pFenceManager;
    }

    VulkanSemaphoreManager* GetSemaphoreManager() const
    {
        return m_pSemaphoreManger;
    }

    VulkanQueue* GetGfxQueue() const
    {
        return m_pGfxQueue;
    }

    VulkanQueue* GetComputeQueue() const
    {
        return m_pComputeQueue;
    }

    VulkanQueue* GetTransferQueue() const
    {
        return m_pTransferQueue;
    }

    VulkanQueue* GetQueue(RHICommandContextType type)
    {
        if (type == RHICommandContextType::eGraphics)
        {
            return m_pGfxQueue;
        }
        if (type == RHICommandContextType::eAsyncCompute)
        {
            return m_pComputeQueue;
        }
        if (type == RHICommandContextType::eTransfer)
        {
            return m_pTransferQueue;
        }
        return m_pGfxQueue;
    }

    const VkPhysicalDeviceFeatures& GetPhysicalDeviceFeatures()
    {
        return m_physicalDeviceFeatures;
    }

    bool SupportsTimelineSemaphore() const
    {
        return m_extensionFlags.hasTimelineSemaphore != 0;
    }

    // LegacyVulkanCommandListContext* GetLegacyImmediateCmdContext() const
    // {
    //     return m_legacyImmediateContext;
    // }
    //
    // LegacyVulkanCommandList* GetLegacyImmediateCommandList() const
    // {
    //     return m_legacyImmediateCommandList;
    // }

    void SubmitCommandsAndFlush();

    void WaitForIdle();

private:
    void SetupDevice(HeapVector<UniquePtr<VulkanDeviceExtension>>& extensions);

    // VulkanRHI* m_RHI;

    VkPhysicalDevice m_gpu{VK_NULL_HANDLE};
    // gpu hardware properties
    VkPhysicalDeviceProperties m_gpuProps{};
    // logical device
    VkDevice m_device{VK_NULL_HANDLE};
    // basic features
    VkPhysicalDeviceFeatures m_physicalDeviceFeatures{};
    // gpu properties
    VkPhysicalDeviceProperties m_physicalDeviceProperties{};
    // descriptor indexing properties
    VkPhysicalDeviceDescriptorIndexingProperties m_descriptorIndexingProperties{};

    DeviceExtensionFlags m_extensionFlags{};

    HeapVector<VkQueueFamilyProperties> m_queueFamilyProps;

    HeapVector<const char*> m_extensions;

    // queue infos
    VulkanQueue* m_pGfxQueue{nullptr};
    VulkanQueue* m_pComputeQueue{nullptr};
    VulkanQueue* m_pTransferQueue{nullptr};

    VulkanFenceManager* m_pFenceManager;
    VulkanSemaphoreManager* m_pSemaphoreManger;
};
} // namespace zen
