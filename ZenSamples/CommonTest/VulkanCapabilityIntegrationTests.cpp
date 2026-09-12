#include "VulkanIntegrationFixture.h"
#include "ScopedVulkanCall.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanMemory.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <memory>
#include <string>
#include "Templates/HeapVector.h"

namespace
{
using namespace zen;

struct CapabilityDriver
{
    static inline PFN_vkGetPhysicalDeviceFeatures2 features;
    static inline PFN_vkGetPhysicalDeviceProperties properties;
    static inline PFN_vkGetPhysicalDeviceProperties2 properties2;
    static inline PFN_vkGetPhysicalDeviceQueueFamilyProperties queues;
    static inline PFN_vkEnumerateDeviceExtensionProperties extensions;
    static inline PFN_vkCreateDevice create;
    static inline bool oldAPI{}, noQueue{}, lowLimits{}, optionalDisabled{};
    static inline bool maintenanceDisabled{};
    static inline uint32_t missingFeature{};
    static inline HeapVector<std::string> hiddenExtensions, enabledExtensions;
    static inline HeapVector<VkStructureType> enabledStructures;
    static inline uint32_t createCalls{};

    static void Reset()
    {
        oldAPI = noQueue = lowLimits = optionalDisabled = false;
        maintenanceDisabled                             = false;
        missingFeature = createCalls = 0;
        hiddenExtensions.clear();
        enabledExtensions.clear();
        enabledStructures.clear();
    }
    static VKAPI_ATTR void VKAPI_CALL Features(VkPhysicalDevice gpu,
                                               VkPhysicalDeviceFeatures2* output)
    {
        features(gpu, output);
        for (auto* node = static_cast<VkBaseOutStructure*>(output->pNext); node; node = node->pNext)
        {
            switch (node->sType)
            {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT:
                    if (maintenanceDisabled)
                    {
                        reinterpret_cast<VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT*>(node)
                            ->swapchainMaintenance1 = VK_FALSE;
                    }
                    break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES:
                    if (missingFeature == 1)
                    {
                        reinterpret_cast<VkPhysicalDeviceDynamicRenderingFeatures*>(node)
                            ->dynamicRendering = VK_FALSE;
                    }
                    break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES:
                {
                    auto* indexing =
                        reinterpret_cast<VkPhysicalDeviceDescriptorIndexingFeatures*>(node);
                    switch (missingFeature)
                    {
                        case 2: indexing->runtimeDescriptorArray = VK_FALSE; break;
                        case 3: indexing->descriptorBindingPartiallyBound = VK_FALSE; break;
                        case 4:
                            indexing->descriptorBindingSampledImageUpdateAfterBind = VK_FALSE;
                            break;
                        case 5:
                            indexing->descriptorBindingUpdateUnusedWhilePending = VK_FALSE;
                            break;
                        case 6:
                            indexing->descriptorBindingVariableDescriptorCount = VK_FALSE;
                            break;
                        case 7:
                            indexing->shaderSampledImageArrayNonUniformIndexing = VK_FALSE;
                            break;
                    }
                    break;
                }
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES:
                    if (optionalDisabled)
                    {
                        reinterpret_cast<VkPhysicalDeviceTimelineSemaphoreFeatures*>(node)
                            ->timelineSemaphore = VK_FALSE;
                    }
                    break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES:
                    if (optionalDisabled)
                    {
                        auto* address =
                            reinterpret_cast<VkPhysicalDeviceBufferDeviceAddressFeatures*>(node);
                        address->bufferDeviceAddress              = VK_FALSE;
                        address->bufferDeviceAddressCaptureReplay = VK_FALSE;
                        address->bufferDeviceAddressMultiDevice   = VK_FALSE;
                    }
                    break;
                default: break;
            }
        }
    }
    static VKAPI_ATTR void VKAPI_CALL Properties(VkPhysicalDevice gpu,
                                                 VkPhysicalDeviceProperties* output)
    {
        properties(gpu, output);
        if (oldAPI)
        {
            output->apiVersion = VK_API_VERSION_1_1;
        }
    }
    static VKAPI_ATTR void VKAPI_CALL Properties2(VkPhysicalDevice gpu,
                                                  VkPhysicalDeviceProperties2* output)
    {
        properties2(gpu, output);
        for (auto* node = static_cast<VkBaseOutStructure*>(output->pNext); node; node = node->pNext)
        {
            if (lowLimits &&
                node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES)
            {
                reinterpret_cast<VkPhysicalDeviceDescriptorIndexingProperties*>(node)
                    ->maxDescriptorSetUpdateAfterBindSampledImages = 1;
            }
        }
    }
    static VKAPI_ATTR void VKAPI_CALL Queues(VkPhysicalDevice gpu,
                                             uint32_t* size,
                                             VkQueueFamilyProperties* output)
    {
        queues(gpu, size, output);
        if (noQueue && output)
        {
            for (uint32_t i = 0; i < *size; ++i)
            {
                output[i].queueFlags &= ~VK_QUEUE_GRAPHICS_BIT;
            }
        }
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Extensions(VkPhysicalDevice gpu,
                                                     const char* layer,
                                                     uint32_t* size,
                                                     VkExtensionProperties* output)
    {
        uint32_t count  = 0;
        VkResult result = extensions(gpu, layer, &count, nullptr);
        if (result != VK_SUCCESS)
        {
            return result;
        }
        HeapVector<VkExtensionProperties> available(count);
        result = extensions(gpu, layer, &count, available.data());
        if (result != VK_SUCCESS)
        {
            return result;
        }
        available.erase(
            std::remove_if(available.begin(), available.end(),
                           [](const auto& extension) {
                               return std::find(hiddenExtensions.begin(), hiddenExtensions.end(),
                                                extension.extensionName) != hiddenExtensions.end();
                           }),
            available.end());
        const uint32_t written = output ? std::min(*size, static_cast<uint32_t>(available.size())) :
                                          static_cast<uint32_t>(available.size());
        if (output)
        {
            std::copy_n(available.data(), written, output);
        }
        *size = written;
        return written == available.size() ? VK_SUCCESS : VK_INCOMPLETE;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Create(VkPhysicalDevice gpu,
                                                 const VkDeviceCreateInfo* info,
                                                 const VkAllocationCallbacks* allocator,
                                                 VkDevice* output)
    {
        ++createCalls;
        for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
        {
            enabledExtensions.emplace_back(info->ppEnabledExtensionNames[i]);
        }
        for (auto* node = static_cast<const VkBaseInStructure*>(info->pNext); node;
             node       = node->pNext)
        {
            enabledStructures.push_back(node->sType);
        }
        return create(gpu, info, allocator, output);
    }
};

class VulkanCapabilityIntegrationTest : public testing::Test
{
protected:
    std::unique_ptr<test::VulkanSession> session;
    HeapVector<std::function<void()>> restore;
    template <typename T> void Hook(T& slot, T replacement, T& previous)
    {
        previous = slot;
        restore.push_back([&slot, old = slot] { slot = old; });
        slot = replacement;
    }
    void SetUp() override
    {
        session = std::make_unique<test::VulkanSession>();
        CapabilityDriver::Reset();
        Hook(vkGetPhysicalDeviceFeatures2, CapabilityDriver::Features, CapabilityDriver::features);
        Hook(vkGetPhysicalDeviceProperties, CapabilityDriver::Properties,
             CapabilityDriver::properties);
        Hook(vkGetPhysicalDeviceProperties2, CapabilityDriver::Properties2,
             CapabilityDriver::properties2);
        Hook(vkGetPhysicalDeviceQueueFamilyProperties, CapabilityDriver::Queues,
             CapabilityDriver::queues);
        Hook(vkEnumerateDeviceExtensionProperties, CapabilityDriver::Extensions,
             CapabilityDriver::extensions);
        Hook(vkCreateDevice, CapabilityDriver::Create, CapabilityDriver::create);
    }
    void TearDown() override
    {
        for (size_t i = restore.size(); i > 0; --i)
        {
            restore[i - 1]();
        }
        // Creating another logical device reloads volk's device-level entry points.
        volkLoadDevice(session->rhi.GetVkDevice());
        session.reset();
        CapabilityDriver::hiddenExtensions  = {};
        CapabilityDriver::enabledExtensions = {};
        CapabilityDriver::enabledStructures = {};
    }
    std::string Reason()
    {
        return VulkanDevice::GetUnsupportedReason(session->rhi.GetPhysicalDevice());
    }
};

struct PipelineCacheFailureDriver
{
    static inline PFN_vkGetDeviceProcAddr original;
    static inline uint32_t calls;
    static VKAPI_ATTR VkResult VKAPI_CALL Create(VkDevice,
                                                 const VkPipelineCacheCreateInfo*,
                                                 const VkAllocationCallbacks*,
                                                 VkPipelineCache* cache)
    {
        ++calls;
        *cache = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Proc(VkDevice device, const char* name)
    {
        return strcmp(name, "vkCreatePipelineCache") == 0 ?
            reinterpret_cast<PFN_vkVoidFunction>(Create) :
            original(device, name);
    }
};

TEST_F(VulkanCapabilityIntegrationTest, OptionalPipelineCacheFailureKeepsDeviceUsable)
{
    PipelineCacheFailureDriver::calls = 0;
    Hook(vkGetDeviceProcAddr, PipelineCacheFailureDriver::Proc,
         PipelineCacheFailureDriver::original);
    VulkanDevice device(session->rhi.GetPhysicalDevice());
    EXPECT_NO_THROW(device.Init());
    EXPECT_EQ(PipelineCacheFailureDriver::calls, 1u);
    EXPECT_NE(device.GetVkHandle(), VK_NULL_HANDLE);
    EXPECT_EQ(device.GetPipelineCache(), VK_NULL_HANDLE);
    device.Destroy();
}

TEST_F(VulkanCapabilityIntegrationTest, RejectsOldAPIAndMissingGraphicsComputeQueue)
{
    ASSERT_TRUE(Reason().empty());
    CapabilityDriver::oldAPI = true;
    EXPECT_NE(Reason().find("Vulkan 1.2"), std::string::npos);
    CapabilityDriver::oldAPI  = false;
    CapabilityDriver::noQueue = true;
    EXPECT_NE(Reason().find("graphics queue"), std::string::npos);
}

TEST_F(VulkanCapabilityIntegrationTest, RejectsMissingRequiredExtensionsAndHeapLimits)
{
    for (const char* extension :
         {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME})
    {
        CapabilityDriver::hiddenExtensions = {extension};
        EXPECT_NE(Reason().find(extension), std::string::npos);
    }
    CapabilityDriver::hiddenExtensions.clear();
    CapabilityDriver::lowLimits = true;
    EXPECT_NE(Reason().find("limits"), std::string::npos);
}

TEST_F(VulkanCapabilityIntegrationTest, MissingRequiredFeatureRejectsBeforeCreatingDevice)
{
    for (uint32_t feature = 1; feature <= 7; ++feature)
    {
        SCOPED_TRACE(feature);
        CapabilityDriver::missingFeature = feature;
        EXPECT_FALSE(Reason().empty());
        VulkanDevice device(session->rhi.GetPhysicalDevice());
        EXPECT_THROW(device.Init(), std::runtime_error);
        device.Destroy();
    }
    EXPECT_EQ(CapabilityDriver::createCalls, 0u);
}

TEST_F(VulkanCapabilityIntegrationTest, PromotedCoreFeaturesDoNotRequireFormerExtensionNames)
{
    CapabilityDriver::hiddenExtensions = {
        VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME, VK_KHR_SPIRV_1_4_EXTENSION_NAME,
        VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME};
    ASSERT_TRUE(Reason().empty());
    VulkanDevice device(session->rhi.GetPhysicalDevice());
    device.Init();
    EXPECT_TRUE(device.GetExtensionFlags().hasDescriptorIndexing);
    for (const auto& name : CapabilityDriver::hiddenExtensions)
    {
        EXPECT_EQ(std::count(CapabilityDriver::enabledExtensions.begin(),
                             CapabilityDriver::enabledExtensions.end(), name),
                  0);
    }
    device.WaitForIdle();
    device.Destroy();
}

TEST_F(VulkanCapabilityIntegrationTest, OptionalFeaturesDisableTheirDependentsAndVMAAddressFlag)
{
    CapabilityDriver::optionalDisabled = true;
    CapabilityDriver::hiddenExtensions = {VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME};
    ASSERT_TRUE(Reason().empty());
    VulkanDevice device(session->rhi.GetPhysicalDevice());
    device.Init();
    EXPECT_FALSE(device.SupportsTimelineSemaphore());
    EXPECT_FALSE(device.GetExtensionFlags().hasBufferDeviceAddress);
    EXPECT_FALSE(device.GetExtensionFlags().hasAccelerationStructure);
    EXPECT_FALSE(device.GetExtensionFlags().hasRaytracingPipeline);
    EXPECT_FALSE(device.GetExtensionFlags().hasRayQuery);
    for (VkStructureType type :
         {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR,
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR})
    {
        EXPECT_EQ(std::count(CapabilityDriver::enabledStructures.begin(),
                             CapabilityDriver::enabledStructures.end(), type),
                  0);
    }
    {
        VulkanMemoryAllocator allocator;
        allocator.Init(session->rhi.GetInstance(), session->rhi.GetPhysicalDevice(),
                       device.GetVkHandle(), device.GetExtensionFlags().hasBufferDeviceAddress);
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size  = 256;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VkBuffer buffer{};
        VulkanMemoryAllocation allocation{};
        allocator.AllocBuffer(256, &info, RHIBufferAllocateType::eCPUWrite, &buffer, &allocation);
        ASSERT_NE(buffer, VK_NULL_HANDLE);
        auto* mapped = reinterpret_cast<uint32_t*>(allocator.MapBuffer(allocation));
        *mapped      = 42;
        EXPECT_EQ(*mapped, 42u);
        allocator.UnmapBuffer(allocation);
        allocator.FreeBuffer(buffer, allocation);
    }
    device.WaitForIdle();
    device.Destroy();
}

TEST_F(VulkanCapabilityIntegrationTest, MissingMaintenanceFeatureUsesUnextendedSwapchain)
{
    CapabilityDriver::maintenanceDisabled = true;
    VulkanDevice device(session->rhi.GetPhysicalDevice());
    device.Init();
    EXPECT_FALSE(device.GetExtensionFlags().hasSwapchainMaintenance1);
    EXPECT_EQ(std::count(CapabilityDriver::enabledStructures.begin(),
                         CapabilityDriver::enabledStructures.end(),
                         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT),
              0);
    for (const auto& name : CapabilityDriver::enabledExtensions)
    {
        EXPECT_NE(name, "VK_KHR_swapchain_maintenance1");
        EXPECT_NE(name, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
    }
    device.Destroy();
}

TEST_F(VulkanCapabilityIntegrationTest, MissingSurfaceMaintenanceDependencyDisablesDeviceExtension)
{
    auto& flags                    = session->rhi.GetInstanceExtensionFlags();
    const auto saved               = flags;
    flags.hasSurfaceMaintenanceKHR = flags.hasSurfaceMaintenanceEXT = 0;
    VulkanDevice device(session->rhi.GetPhysicalDevice());
    device.Init();
    EXPECT_FALSE(device.GetExtensionFlags().hasSwapchainMaintenance1);
    EXPECT_EQ(std::count(CapabilityDriver::enabledStructures.begin(),
                         CapabilityDriver::enabledStructures.end(),
                         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT),
              0);
    flags = saved;
    device.Destroy();
}

TEST_F(VulkanCapabilityIntegrationTest, MaintenanceSelectsEXTWhenKHRNameIsAbsent)
{
    const auto& flags = session->rhi.GetInstanceExtensionFlags();
    ASSERT_TRUE(flags.hasSurfaceMaintenanceEXT);
    CapabilityDriver::hiddenExtensions = {"VK_KHR_swapchain_maintenance1"};
    VulkanDevice device(session->rhi.GetPhysicalDevice());
    device.Init();
    EXPECT_TRUE(device.GetExtensionFlags().hasSwapchainMaintenance1);
    EXPECT_EQ(std::count(CapabilityDriver::enabledExtensions.begin(),
                         CapabilityDriver::enabledExtensions.end(),
                         VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME),
              1);
    EXPECT_EQ(std::count(CapabilityDriver::enabledExtensions.begin(),
                         CapabilityDriver::enabledExtensions.end(),
                         "VK_KHR_swapchain_maintenance1"),
              0);
    device.Destroy();
}

TEST_F(VulkanCapabilityIntegrationTest, AbsentMaintenanceNamesKeepDeviceUsable)
{
    CapabilityDriver::hiddenExtensions = {"VK_KHR_swapchain_maintenance1",
                                          VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME};
    VulkanDevice device(session->rhi.GetPhysicalDevice());
    EXPECT_NO_THROW(device.Init());
    EXPECT_FALSE(device.GetExtensionFlags().hasSwapchainMaintenance1);
    EXPECT_EQ(std::count(CapabilityDriver::enabledStructures.begin(),
                         CapabilityDriver::enabledStructures.end(),
                         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT),
              0);
    device.Destroy();
}

struct InstanceMaintenanceDriver
{
    static inline PFN_vkEnumerateInstanceExtensionProperties original;
    static VKAPI_ATTR VkResult VKAPI_CALL Extensions(const char* layer,
                                                     uint32_t* count,
                                                     VkExtensionProperties* output)
    {
        uint32_t size   = 0;
        VkResult result = original(layer, &size, nullptr);
        if (result != VK_SUCCESS)
        {
            return result;
        }
        HeapVector<VkExtensionProperties> available(size);
        result = original(layer, &size, available.data());
        if (result != VK_SUCCESS)
        {
            return result;
        }
        available.resize(size);
        available.erase(
            std::remove_if(available.begin(), available.end(),
                           [](const auto& extension) {
                               return strcmp(extension.extensionName,
                                             VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) == 0;
                           }),
            available.end());
        if (output)
        {
            const size_t written = std::min(size_t(*count), available.size());
            std::copy_n(available.begin(), written, output);
            *count = static_cast<uint32_t>(written);
            return written < available.size() ? VK_INCOMPLETE : VK_SUCCESS;
        }
        *count = static_cast<uint32_t>(available.size());
        return VK_SUCCESS;
    }
};

TEST_F(VulkanCapabilityIntegrationTest, SurfaceMaintenanceRequiresSurfaceCapabilities2)
{
    InstanceMaintenanceDriver::original = vkEnumerateInstanceExtensionProperties;
    test::ScopedVulkanCall hook(vkEnumerateInstanceExtensionProperties,
                                InstanceMaintenanceDriver::Extensions);
    InstanceExtensionFlags flags{};
    const auto extensions = VulkanInstanceExtension::GetEnabledInstanceExtensions(flags);
    EXPECT_FALSE(flags.hasSurfaceMaintenanceKHR);
    EXPECT_FALSE(flags.hasSurfaceMaintenanceEXT);
    for (const auto& extension : extensions)
    {
        if (extension->GetName() == NameID("VK_KHR_surface_maintenance1") ||
            extension->GetName() == NameID(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME))
        {
            EXPECT_FALSE(extension->IsEnabledAndSupported());
        }
    }
}
} // namespace
