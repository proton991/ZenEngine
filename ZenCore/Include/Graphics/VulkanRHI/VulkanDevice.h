#pragma once
#include "VulkanHeaders.h"
#include "Common/UniquePtr.h"
#include <vector>
#include <string>

namespace zen::rhi
{
class VulkanRHI;
class VulkanDeviceExtension;
class VulkanQueue;

struct DeviceExtensionFlags
{
    uint32_t hasBufferDeviceAddress : 1;
    uint32_t hasAccelerationStructure : 1;
    uint32_t hasRaytracingPipeline : 1;
    uint32_t hasRayQuery : 1;
    uint32_t hasDescriptorIndexing : 1;

    uint32_t hasDeferredHostOperation : 1;
    uint32_t hasSPIRV_14 : 1;
};

class VulkanDevice
{
public:
    VulkanDevice(VulkanRHI* RHI, VkPhysicalDevice gpu);

    void Init();

    void Destroy();

    VkDevice GetVkHandle() const { return m_device; }

    VkPhysicalDeviceProperties GetPhysicalDeviceProperties() const { return m_gpuProps; }

    DeviceExtensionFlags& GetExtensionFlags() { return m_extensionFlags; }

    VkPhysicalDevice GetPhysicalDeviceHandle() const { return m_gpu; }

    void SetObjectName(VkObjectType type, uint64_t handle, const char* name);

    const auto& GetDescriptorIndexingProperties() const { return m_descriptorIndexingProperties; }

private:
    void SetupDevice(std::vector<UniquePtr<VulkanDeviceExtension>>& extensions);

    VulkanRHI* m_RHI;

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

    std::vector<VkQueueFamilyProperties> m_queueFamilyProps;

    std::vector<const char*> m_extensions;

    // queue infos
    VulkanQueue* m_gfxQueue{nullptr};
    VulkanQueue* m_computeQueue{nullptr};
    VulkanQueue* m_transferQueue{nullptr};
};
} // namespace zen::rhi