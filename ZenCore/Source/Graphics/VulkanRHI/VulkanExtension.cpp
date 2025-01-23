#include "Common/UniquePtr.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"
#include "Graphics/VulkanRHI/VulkanExtension.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"

#if defined(ZEN_WIN32)
#    include "Graphics/VulkanRHI/Platform/VulkanWindowsPlatform.h"
#endif

#if defined(ZEN_MACOS)
#    include "Graphics/VulkanRHI/Platform/VulkanMacOSPlatform.h"
#endif

namespace zen::rhi
{

template <typename ExistingChainType, typename NewStructType>
static void AddToPNext(ExistingChainType& Existing, NewStructType& Added)
{
    Added.pNext    = (void*)Existing.pNext;
    Existing.pNext = (void*)&Added;
}

// Advanced extensions
/**
 * VK_EXT_descriptor_indexing
 */
class VulkanDescriptorIndexingExtension : public VulkanDeviceExtension
{
public:
    VulkanDescriptorIndexingExtension(VulkanDevice* device) :
        VulkanDeviceExtension(device, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME)
    {
        InitVkStruct(m_descriptorIndexingFeatures,
                     VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES);
    }

    void BeforePhysicalDeviceFeatures(
        VkPhysicalDeviceFeatures2KHR& physicalDeviceFeatures2Khr) final
    {

        AddToPNext(physicalDeviceFeatures2Khr, m_descriptorIndexingFeatures);
    }

    virtual void AfterPhysicalDeviceFeatures() final
    {
        bool supported = (m_descriptorIndexingFeatures.runtimeDescriptorArray == VK_TRUE) &&
            (m_descriptorIndexingFeatures.descriptorBindingPartiallyBound == VK_TRUE) &&
            (m_descriptorIndexingFeatures.descriptorBindingUpdateUnusedWhilePending == VK_TRUE) &&
            (m_descriptorIndexingFeatures.descriptorBindingVariableDescriptorCount == VK_TRUE);
        if (supported)
        {
            SetSupport();
            m_device->GetExtensionFlags().hasDescriptorIndexing = 1;
        }
    }

    void BeforeCreateDevice(VkDeviceCreateInfo& DeviceCI) final
    {
        if (IsEnabledAndSupported())
        {
            AddToPNext(DeviceCI, m_descriptorIndexingFeatures);
        }
    }

private:
    VkPhysicalDeviceDescriptorIndexingFeatures m_descriptorIndexingFeatures;
};

/**
 * VK_KHR_dynamic_rendering
 */
class VulkanDynamicRenderingExtension : public VulkanDeviceExtension
{
public:
    VulkanDynamicRenderingExtension(VulkanDevice* device) :
        VulkanDeviceExtension(device, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME)
    {
        InitVkStruct(m_dynamicRenderingFeaturesKHR,
                     VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR);
    }

    void BeforePhysicalDeviceFeatures(
        VkPhysicalDeviceFeatures2KHR& physicalDeviceFeatures2Khr) final
    {
        AddToPNext(physicalDeviceFeatures2Khr, m_dynamicRenderingFeaturesKHR);
    }

    void AfterPhysicalDeviceFeatures() final
    {
        bool supported = (m_dynamicRenderingFeaturesKHR.dynamicRendering == VK_TRUE);
        if (supported)
        {
            SetSupport();
            m_device->GetExtensionFlags().hasDynamicRendering = 1;
        }
    }

    void BeforeCreateDevice(VkDeviceCreateInfo& DeviceCI) final
    {
        if (IsEnabledAndSupported())
        {
            AddToPNext(DeviceCI, m_dynamicRenderingFeaturesKHR);
        }
    }

private:
    VkPhysicalDeviceDynamicRenderingFeaturesKHR m_dynamicRenderingFeaturesKHR;
};

/**
 * VK_KHR_buffer_device_address
 */
class VulkanBufferDeviceAddressExtension : public VulkanDeviceExtension
{
public:
    VulkanBufferDeviceAddressExtension(VulkanDevice* device) :
        VulkanDeviceExtension(device, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME)
    {
        InitVkStruct(m_bufferDeviceAddressFeature,
                     VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES);
    }

    void BeforePhysicalDeviceFeatures(
        VkPhysicalDeviceFeatures2KHR& physicalDeviceFeatures2Khr) final
    {
        AddToPNext(physicalDeviceFeatures2Khr, m_bufferDeviceAddressFeature);
    }

    virtual void AfterPhysicalDeviceFeatures() final
    {
        bool supported = (m_bufferDeviceAddressFeature.bufferDeviceAddress == VK_TRUE);
        if (supported)
        {
            SetSupport();
            m_device->GetExtensionFlags().hasBufferDeviceAddress = 1;
        }
    }

    void BeforeCreateDevice(VkDeviceCreateInfo& DeviceCI) final
    {
        if (IsEnabledAndSupported())
        {
            AddToPNext(DeviceCI, m_bufferDeviceAddressFeature);
        }
    }

private:
    VkPhysicalDeviceBufferDeviceAddressFeatures m_bufferDeviceAddressFeature;
};

/**
 * VK_KHR_acceleration_structure
 */
class VulkanAccelerationStructureExtension : public VulkanDeviceExtension
{
public:
    VulkanAccelerationStructureExtension(VulkanDevice* device) :
        VulkanDeviceExtension(device, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)
    {
        InitVkStruct(m_accelerationStructureFeatures,
                     VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR);
    }

    void BeforePhysicalDeviceFeatures(
        VkPhysicalDeviceFeatures2KHR& physicalDeviceFeatures2Khr) final
    {
        AddToPNext(physicalDeviceFeatures2Khr, m_accelerationStructureFeatures);
    }

    virtual void AfterPhysicalDeviceFeatures() final
    {
        bool supported = (m_accelerationStructureFeatures.accelerationStructure == VK_TRUE);
        if (supported)
        {
            SetSupport();
            m_device->GetExtensionFlags().hasAccelerationStructure = 1;
        }
    }

    void BeforeCreateDevice(VkDeviceCreateInfo& DeviceCI) final
    {
        if (IsEnabledAndSupported())
        {
            AddToPNext(DeviceCI, m_accelerationStructureFeatures);
        }
    }

private:
    VkPhysicalDeviceAccelerationStructureFeaturesKHR m_accelerationStructureFeatures;
};

/**
 * VK_KHR_ray_tracing_pipeline
 */
class VulkanRaytracingPipelineExtension : public VulkanDeviceExtension
{
public:
    VulkanRaytracingPipelineExtension(VulkanDevice* device) :
        VulkanDeviceExtension(device, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME)
    {
        InitVkStruct(m_rayTracingPipelineFeatures,
                     VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR);
    }

    void BeforePhysicalDeviceFeatures(
        VkPhysicalDeviceFeatures2KHR& physicalDeviceFeatures2Khr) final
    {
        AddToPNext(physicalDeviceFeatures2Khr, m_rayTracingPipelineFeatures);
    }

    virtual void AfterPhysicalDeviceFeatures() final
    {
        bool supported = (m_rayTracingPipelineFeatures.rayTracingPipeline == VK_TRUE);
        if (supported)
        {
            SetSupport();
            m_device->GetExtensionFlags().hasRaytracingPipeline = 1;
        }
    }

    void BeforeCreateDevice(VkDeviceCreateInfo& DeviceCI) final
    {
        if (IsEnabledAndSupported())
        {
            AddToPNext(DeviceCI, m_rayTracingPipelineFeatures);
        }
    }

private:
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR m_rayTracingPipelineFeatures;
};

/**
 * VK_KHR_ray_query
 */
class VulkanRayQueryExtension : public VulkanDeviceExtension
{
public:
    VulkanRayQueryExtension(VulkanDevice* device) :
        VulkanDeviceExtension(device, VK_KHR_RAY_QUERY_EXTENSION_NAME)
    {
        InitVkStruct(m_rayQueryFeature, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR);
    }

    void BeforePhysicalDeviceFeatures(
        VkPhysicalDeviceFeatures2KHR& physicalDeviceFeatures2Khr) final
    {
        AddToPNext(physicalDeviceFeatures2Khr, m_rayQueryFeature);
    }

    virtual void AfterPhysicalDeviceFeatures() final
    {
        bool supported = (m_rayQueryFeature.rayQuery == VK_TRUE);
        if (supported)
        {
            SetSupport();
            m_device->GetExtensionFlags().hasRayQuery = 1;
        }
    }

    void BeforeCreateDevice(VkDeviceCreateInfo& DeviceCI) final
    {
        if (IsEnabledAndSupported())
        {
            AddToPNext(DeviceCI, m_rayQueryFeature);
        }
    }

private:
    VkPhysicalDeviceRayQueryFeaturesKHR m_rayQueryFeature;
};

static int FindExtensionIndex(const char* extensionName,
                              const std::vector<VkExtensionProperties>& supported)
{
    auto it = std::find_if(supported.begin(), supported.end(),
                           [&extensionName](const VkExtensionProperties& extProp) {
                               return strcmp(extensionName, extProp.extensionName) == 0;
                           });
    if (it == supported.end())
    {
        LOGW("Instance extension {} not supported!", extensionName);
        return -1;
    }
    return static_cast<int>(it - supported.begin());
}

template <class ExtensionType>
static void FlagExtensionSupported(std::vector<ExtensionType>& extensions,
                                   const std::vector<VkExtensionProperties>& supported)
{
    for (ExtensionType& extension : extensions)
    {
        int index = FindExtensionIndex(extension->GetName(), supported);
        if (index != -1)
        {
            extension->SetSupport();
        }
    }
}

std::vector<VkExtensionProperties> VulkanInstanceExtension::GetSupportedInstanceExtensions(
    const char* layerName)
{
    std::vector<VkExtensionProperties> extensions;

    uint32_t count = 0;
    VKCHECK(vkEnumerateInstanceExtensionProperties(layerName, &count, nullptr));
    if (count > 0)
    {
        extensions.resize(count);
        VKCHECK(vkEnumerateInstanceExtensionProperties(layerName, &count, extensions.data()));
    }
    std::sort(extensions.begin(), extensions.end(),
              [](const VkExtensionProperties& a, const VkExtensionProperties& b) {
                  return strcmp(a.extensionName, b.extensionName) < 0;
              });
    return extensions;
}

VulkanInstanceExtensionArray VulkanInstanceExtension::GetEnabledInstanceExtensions(
    InstanceExtensionFlags& extensionFlags)
{
    VulkanInstanceExtensionArray enabledExtensions;

#define SET_INSTANCE_EXTENSION_FLAG(FLAG_NAME) extensionFlags.FLAG_NAME = 1;
#define ADD_INSTANCE_EXTENSION(EXTENSION_NAME) \
    enabledExtensions.emplace_back(MakeUnique<VulkanInstanceExtension>(EXTENSION_NAME));

    // generic extensions
    ADD_INSTANCE_EXTENSION(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    SET_INSTANCE_EXTENSION_FLAG(hasGetPhysicalDeviceProperties);
    ADD_INSTANCE_EXTENSION(VK_KHR_SURFACE_EXTENSION_NAME);
    ADD_INSTANCE_EXTENSION(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    VulkanPlatform::AddInstanceExtensions(enabledExtensions);

    FlagExtensionSupported(enabledExtensions,
                           VulkanInstanceExtension::GetSupportedInstanceExtensions());

#undef SET_INSTANCE_EXTENSION_FLAG
#undef ADD_INSTANCE_EXTENSION

    return enabledExtensions;
}

std::vector<VkExtensionProperties> VulkanDeviceExtension::GetSupportedExtensions(
    VkPhysicalDevice gpu)
{
    std::vector<VkExtensionProperties> extensions;

    uint32_t count = 0;
    VKCHECK(vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, nullptr));
    if (count > 0)
    {
        extensions.resize(count);
        VKCHECK(vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, extensions.data()));
    }
    std::sort(extensions.begin(), extensions.end(),
              [](const VkExtensionProperties& a, const VkExtensionProperties& b) {
                  return strcmp(a.extensionName, b.extensionName) < 0;
              });
    return extensions;
}

VulkanDeviceExtensionArray VulkanDeviceExtension::GetEnabledExtensions(VulkanDevice* device)
{
    VulkanDeviceExtensionArray enabledExtensions;

#define SET_SIMPLE_EXTENSION_FLAG(FLAG_NAME) device->GetExtensionFlags().FLAG_NAME = 1;

#define ADD_SIMPLE_DEVICE_EXTENSION(EXTENSION_NAME) \
    enabledExtensions.emplace_back(MakeUnique<VulkanDeviceExtension>(device, EXTENSION_NAME));

#define ADD_ADVANCED_DEVICE_EXTENSION(EXTENSION_CLASS) \
    enabledExtensions.emplace_back(MakeUnique<EXTENSION_CLASS>(device));

#if defined(ZEN_MACOS)
    ADD_SIMPLE_DEVICE_EXTENSION("VK_KHR_portability_subset");
#endif

    ADD_SIMPLE_DEVICE_EXTENSION(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    ADD_SIMPLE_DEVICE_EXTENSION(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    ADD_SIMPLE_DEVICE_EXTENSION(VK_KHR_SPIRV_1_4_EXTENSION_NAME);

    SET_SIMPLE_EXTENSION_FLAG(hasDeferredHostOperation)
    SET_SIMPLE_EXTENSION_FLAG(hasSPIRV_14)

    ADD_ADVANCED_DEVICE_EXTENSION(VulkanDescriptorIndexingExtension)

    // ray tracing extensions
    ADD_ADVANCED_DEVICE_EXTENSION(VulkanBufferDeviceAddressExtension)
    ADD_ADVANCED_DEVICE_EXTENSION(VulkanAccelerationStructureExtension)
    ADD_ADVANCED_DEVICE_EXTENSION(VulkanRaytracingPipelineExtension)
    ADD_ADVANCED_DEVICE_EXTENSION(VulkanRayQueryExtension)
    ADD_ADVANCED_DEVICE_EXTENSION(VulkanDynamicRenderingExtension)

    FlagExtensionSupported(
        enabledExtensions,
        VulkanDeviceExtension::GetSupportedExtensions(device->GetPhysicalDeviceHandle()));
#undef SET_EXTENSION_FLAG
#undef ADD_SIMPLE_DEVICE_EXTENSION
#undef ADD_ADVANCED_DEVICE_EXTENSION

    return enabledExtensions;
}



} // namespace zen::rhi