#pragma once
#include "VulkanHeaders.h"
#include "Graphics/RHI/RHICommandList.h"
#include "Utils/UniquePtr.h"
#include "Templates/HeapVector.h"
#include <string>

namespace zen
{
class VulkanRHI;
class VulkanDeviceExtension;
class VulkanQueue;
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

    // Empty means the device satisfies the backend's minimum capability profile.
    static std::string GetUnsupportedReason(VkPhysicalDevice gpu);

    void Init();

    void Destroy();

    VkDevice GetVkHandle() const
    {
        return m_device;
    }

    VkPipelineCache GetPipelineCache() const
    {
        return m_pipelineCache;
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

    void SetObjectName(VkObjectType type, uint64_t handle, NameID name);

    const VkPhysicalDeviceDescriptorIndexingProperties& GetDescriptorIndexingProperties() const
    {
        return m_descriptorIndexingProperties;
    }

    uint32_t GetDescriptorSetUpdateAfterBindLimit(VkDescriptorType descriptorType) const;

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

    VulkanQueue* GetQueue(RHICommandContextType type) const
    {
        VulkanQueue* result{};

        if (type == RHICommandContextType::eGraphics)
        {
            result = m_pGfxQueue;
        }
        else if (type == RHICommandContextType::eAsyncCompute)
        {
            result = m_pComputeQueue;
        }
        else if (type == RHICommandContextType::eTransfer)
        {
            result = m_pTransferQueue;
        }
        else
        {
            result = m_pGfxQueue;
        }

        return result;
    }

    const VkQueueFamilyProperties& GetQueueFamilyProperties(uint32_t familyIndex) const
    {
        return m_queueFamilyProps[familyIndex];
    }

    const VkPhysicalDeviceFeatures& GetPhysicalDeviceFeatures()
    {
        return m_physicalDeviceFeatures;
    }

    bool SupportsTimelineSemaphore() const
    {
        return m_extensionFlags.hasTimelineSemaphore != 0;
    }

    void WaitForIdle();

private:

    void SetupDevice(HeapVector<UniquePtr<VulkanDeviceExtension>>& extensions);

    VkPhysicalDevice m_gpu{VK_NULL_HANDLE};

    // gpu hardware properties
    VkPhysicalDeviceProperties m_gpuProps{};

    // logical device
    VkDevice m_device{VK_NULL_HANDLE};
    VkPipelineCache m_pipelineCache{VK_NULL_HANDLE};

    // basic features
    VkPhysicalDeviceFeatures m_physicalDeviceFeatures{};

    // gpu properties
    VkPhysicalDeviceProperties m_physicalDeviceProperties{};

    // descriptor indexing properties
    VkPhysicalDeviceDescriptorIndexingProperties m_descriptorIndexingProperties{};

    DeviceExtensionFlags m_extensionFlags{};

    HeapVector<VkQueueFamilyProperties> m_queueFamilyProps;

    HeapVector<NameID> m_extensions;

    // queue infos
    VulkanQueue* m_pGfxQueue{nullptr};
    VulkanQueue* m_pComputeQueue{nullptr};
    VulkanQueue* m_pTransferQueue{nullptr};

    VulkanFenceManager* m_pFenceManager{nullptr};
    VulkanSemaphoreManager* m_pSemaphoreManger{nullptr};
};
} // namespace zen
