#include "VulkanIntegrationFixture.h"
#include "ScopedVulkanCall.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanQueue.h"
#include "Graphics/VulkanRHI/VulkanMemory.h"
#include "Graphics/VulkanRHI/VulkanSynchronization.h"
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
    static inline uint32_t forcedGraphicsFamily{UINT32_MAX};
    static inline uint32_t forcedQueueCount{0};
    static inline HeapVector<VkDeviceQueueCreateInfo> requestedQueues;

    static void Reset()
    {
        oldAPI = noQueue = lowLimits = optionalDisabled = false;
        maintenanceDisabled                             = false;
        missingFeature = createCalls = 0;
        forcedGraphicsFamily         = UINT32_MAX;
        forcedQueueCount             = 0;
        requestedQueues.clear();
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
        if (output != nullptr && forcedGraphicsFamily != UINT32_MAX)
        {
            for (uint32_t i = 0; i < *size; ++i)
            {
                output[i].queueCount = i == forcedGraphicsFamily ?
                    std::min(output[i].queueCount, forcedQueueCount) :
                    0;
            }
        }
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
        for (uint32_t i = 0; i < info->queueCreateInfoCount; ++i)
        {
            VkDeviceQueueCreateInfo queue = info->pQueueCreateInfos[i];
            EXPECT_NE(queue.pQueuePriorities, nullptr);
            for (uint32_t j = 0; j < queue.queueCount; ++j)
            {
                EXPECT_GE(queue.pQueuePriorities[j], 0.0f);
                EXPECT_LE(queue.pQueuePriorities[j], 1.0f);
            }
            queue.pQueuePriorities = nullptr;
            requestedQueues.push_back(queue);
        }
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
        CapabilityDriver::requestedQueues   = {};
    }
    std::string Reason()
    {
        return VulkanDevice::GetUnsupportedReason(session->rhi.GetPhysicalDevice());
    }
};

TEST_F(VulkanCapabilityIntegrationTest, QueueCapabilitiesMatchActualHandlesAndTimelineSupport)
{
    VulkanDevice* device                    = session->rhi.GetDevice();
    const RHIQueueCapabilities capabilities = session->rhi.GetQueueCapabilities();
    EXPECT_TRUE(capabilities.computeSupported);
    EXPECT_EQ(capabilities.asyncSubmissionDependencies, device->SupportsTimelineSemaphore());
    for (uint32_t i = 0; i < static_cast<uint32_t>(RHICommandContextType::eMax); ++i)
    {
        for (uint32_t j = 0; j < static_cast<uint32_t>(RHICommandContextType::eMax); ++j)
        {
            const RHICommandContextType first  = static_cast<RHICommandContextType>(i);
            const RHICommandContextType second = static_cast<RHICommandContextType>(j);
            EXPECT_EQ(capabilities.AreQueuesShared(first, second),
                      device->GetQueue(first)->GetVkHandle() ==
                          device->GetQueue(second)->GetVkHandle());
        }
    }
    const uint32_t timelineSupported = device->GetExtensionFlags().hasTimelineSemaphore;
    device->GetExtensionFlags().hasTimelineSemaphore = 0;
    EXPECT_FALSE(session->rhi.GetQueueCapabilities().asyncSubmissionDependencies);
    EXPECT_FALSE(session->rhi.GetQueueCapabilities().SupportsAsyncCompute());
    device->GetExtensionFlags().hasTimelineSemaphore = timelineSupported;
}

TEST_F(VulkanCapabilityIntegrationTest, SingleNativeQueueReportsSharedFallback)
{
    CapabilityDriver::forcedGraphicsFamily =
        session->rhi.GetDevice()->GetGfxQueue()->GetFamilyIndex();
    CapabilityDriver::forcedQueueCount = 1;
    VulkanDevice device(session->rhi.GetPhysicalDevice());
    device.Init();
    const RHIQueueCapabilities capabilities = device.GetQueueCapabilities();
    EXPECT_TRUE(capabilities.computeSupported);
    EXPECT_FALSE(capabilities.SupportsAsyncCompute());
    EXPECT_EQ(device.GetGfxQueue()->GetVkHandle(), device.GetComputeQueue()->GetVkHandle());
    EXPECT_EQ(device.GetComputeQueue()->GetVkHandle(), device.GetTransferQueue()->GetVkHandle());
    EXPECT_TRUE(capabilities.AreQueuesShared(RHICommandContextType::eGraphics,
                                             RHICommandContextType::eAsyncCompute));
    EXPECT_TRUE(capabilities.AreQueuesShared(RHICommandContextType::eAsyncCompute,
                                             RHICommandContextType::eTransfer));
    EXPECT_EQ(CapabilityDriver::requestedQueues.size(), 1u);
    for (const VkDeviceQueueCreateInfo& queue : CapabilityDriver::requestedQueues)
    {
        EXPECT_EQ(queue.queueFamilyIndex, CapabilityDriver::forcedGraphicsFamily);
        EXPECT_EQ(queue.queueCount, 1u);
    }
    device.Destroy();
}

TEST_F(VulkanCapabilityIntegrationTest, DistinctIndicesInOneFamilyCreateIndependentNativeQueues)
{
    const VulkanDevice* existing = session->rhi.GetDevice();
    const uint32_t family        = existing->GetGfxQueue()->GetFamilyIndex();
    if (existing->GetQueueFamilyProperties(family).queueCount < 2)
    {
        GTEST_SKIP() << "Requires at least two queues in the graphics/compute family";
    }
    CapabilityDriver::forcedGraphicsFamily = family;
    CapabilityDriver::forcedQueueCount     = 2;
    VulkanDevice device(session->rhi.GetPhysicalDevice());
    device.Init();
    EXPECT_EQ(device.GetGfxQueue()->GetFamilyIndex(), family);
    EXPECT_EQ(device.GetComputeQueue()->GetFamilyIndex(), family);
    EXPECT_EQ(device.GetGfxQueue()->GetQueueIndex(), 0u);
    EXPECT_EQ(device.GetComputeQueue()->GetQueueIndex(), 1u);
    EXPECT_NE(device.GetGfxQueue()->GetVkHandle(), device.GetComputeQueue()->GetVkHandle());
    EXPECT_EQ(device.GetComputeQueue()->GetVkHandle(), device.GetTransferQueue()->GetVkHandle());
    const RHIQueueCapabilities capabilities = device.GetQueueCapabilities();
    EXPECT_FALSE(capabilities.AreQueuesShared(RHICommandContextType::eGraphics,
                                              RHICommandContextType::eAsyncCompute));
    EXPECT_TRUE(capabilities.AreQueuesShared(RHICommandContextType::eAsyncCompute,
                                             RHICommandContextType::eTransfer));
    EXPECT_EQ(capabilities.SupportsAsyncCompute(), device.SupportsTimelineSemaphore());
    EXPECT_EQ(CapabilityDriver::requestedQueues.size(), 1u);
    for (const VkDeviceQueueCreateInfo& queue : CapabilityDriver::requestedQueues)
    {
        EXPECT_EQ(queue.queueFamilyIndex, family);
        EXPECT_EQ(queue.queueCount, 2u);
    }
    device.Destroy();
}

struct ScopedTopologyDevice
{
    VulkanDevice device;
    explicit ScopedTopologyDevice(VkPhysicalDevice physical) : device(physical)
    {
        device.Init();
    }
    ~ScopedTopologyDevice()
    {
        device.WaitForIdle();
        device.Destroy();
    }
};

struct TopologyReadbackBuffer
{
    VkDevice device;
    VkBuffer buffer{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    explicit TopologyReadbackBuffer(VkDevice owner) : device(owner) {}
    ~TopologyReadbackBuffer()
    {
        vkDestroyBuffer(device, buffer, nullptr);
        vkFreeMemory(device, memory, nullptr);
    }
    bool Init(VkPhysicalDevice physical)
    {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size        = 8;
        info.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        bool valid       = vkCreateBuffer(device, &info, nullptr, &buffer) == VK_SUCCESS;
        if (valid)
        {
            VkMemoryRequirements requirements{};
            vkGetBufferMemoryRequirements(device, buffer, &requirements);
            VkPhysicalDeviceMemoryProperties properties{};
            vkGetPhysicalDeviceMemoryProperties(physical, &properties);
            uint32_t type = UINT32_MAX;
            constexpr VkMemoryPropertyFlags flags =
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
            {
                if ((requirements.memoryTypeBits & (1u << i)) &&
                    (properties.memoryTypes[i].propertyFlags & flags) == flags)
                {
                    type = i;
                    break;
                }
            }
            valid = type != UINT32_MAX;
            if (valid)
            {
                VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                allocation.allocationSize  = requirements.size;
                allocation.memoryTypeIndex = type;
                valid = vkAllocateMemory(device, &allocation, nullptr, &memory) == VK_SUCCESS &&
                    vkBindBufferMemory(device, buffer, memory, 0) == VK_SUCCESS;
            }
        }
        return valid;
    }
};

struct TopologyCommandPool
{
    VkDevice device;
    VkCommandPool pool{VK_NULL_HANDLE};
    VkCommandBuffer buffers[2]{};
    explicit TopologyCommandPool(VkDevice owner) : device(owner) {}
    ~TopologyCommandPool()
    {
        vkDeviceWaitIdle(device);
        vkDestroyCommandPool(device, pool, nullptr);
    }
    bool Init(uint32_t family)
    {
        VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        info.queueFamilyIndex = family;
        bool valid            = vkCreateCommandPool(device, &info, nullptr, &pool) == VK_SUCCESS;
        if (valid)
        {
            VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocate.commandPool        = pool;
            allocate.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 2;
            valid = vkAllocateCommandBuffers(device, &allocate, buffers) == VK_SUCCESS;
        }
        return valid;
    }
};

struct TopologySubmitObserver
{
    static inline PFN_vkQueueSubmit original;
    static inline PFN_vkCmdPipelineBarrier originalBarrier;
    static inline uint32_t waits;
    static VKAPI_ATTR VkResult VKAPI_CALL Submit(VkQueue queue,
                                                 uint32_t count,
                                                 const VkSubmitInfo* submissions,
                                                 VkFence fence)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            waits += submissions[i].waitSemaphoreCount;
            for (uint32_t j = 0; j < submissions[i].waitSemaphoreCount; ++j)
            {
                EXPECT_EQ(submissions[i].pWaitDstStageMask[j], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            }
        }
        return original(queue, count, submissions, fence);
    }
    static VKAPI_ATTR void VKAPI_CALL Barrier(VkCommandBuffer commands,
                                              VkPipelineStageFlags source,
                                              VkPipelineStageFlags destination,
                                              VkDependencyFlags flags,
                                              uint32_t memoryCount,
                                              const VkMemoryBarrier* memory,
                                              uint32_t bufferCount,
                                              const VkBufferMemoryBarrier* buffers,
                                              uint32_t imageCount,
                                              const VkImageMemoryBarrier* images)
    {
        for (uint32_t i = 0; i < bufferCount; ++i)
        {
            EXPECT_EQ(buffers[i].srcQueueFamilyIndex, VK_QUEUE_FAMILY_IGNORED);
            EXPECT_EQ(buffers[i].dstQueueFamilyIndex, VK_QUEUE_FAMILY_IGNORED);
        }
        originalBarrier(commands, source, destination, flags, memoryCount, memory, bufferCount,
                        buffers, imageCount, images);
    }
};

TEST_F(VulkanCapabilityIntegrationTest, SameFamilyHandoffUsesWaitForDistinctQueueAndBarrierForAlias)
{
    const VulkanDevice* existing = session->rhi.GetDevice();
    const uint32_t family        = existing->GetGfxQueue()->GetFamilyIndex();
    if (existing->GetQueueFamilyProperties(family).queueCount < 2)
    {
        GTEST_SKIP() << "Requires two available queues in one graphics/compute family";
    }
    CapabilityDriver::forcedGraphicsFamily = family;
    CapabilityDriver::forcedQueueCount     = 2;
    ScopedTopologyDevice owner(session->rhi.GetPhysicalDevice());
    VulkanDevice& device = owner.device;
    EXPECT_NE(device.GetGfxQueue()->GetVkHandle(), device.GetComputeQueue()->GetVkHandle());
    EXPECT_EQ(device.GetTransferQueue()->GetVkHandle(), device.GetComputeQueue()->GetVkHandle());
    TopologyReadbackBuffer data(device.GetVkHandle());
    ASSERT_TRUE(data.Init(session->rhi.GetPhysicalDevice()));
    // Engine command pools use the active RHI device. This isolated secondary device owns
    // its native pools directly, while using the engine's selected queue handles and barriers.
    VulkanSemaphore semaphore(&device);
    TopologyCommandPool commands(device.GetVkHandle());
    ASSERT_TRUE(commands.Init(family));
    TopologySubmitObserver::originalBarrier = vkCmdPipelineBarrier;
    test::ScopedVulkanCall<PFN_vkCmdPipelineBarrier> observeBarriers(
        vkCmdPipelineBarrier, &TopologySubmitObserver::Barrier);
    for (bool alias : {false, true})
    {
        void* initial = nullptr;
        ASSERT_EQ(vkMapMemory(device.GetVkHandle(), data.memory, 0, 8, 0, &initial), VK_SUCCESS);
        static_cast<uint32_t*>(initial)[0] = 0;
        static_cast<uint32_t*>(initial)[1] = 0xdeadbeef;
        vkUnmapMemory(device.GetVkHandle(), data.memory);
        VulkanQueue* producer = device.GetQueue(alias ? RHICommandContextType::eTransfer :
                                                        RHICommandContextType::eGraphics);
        VulkanQueue* consumer = device.GetComputeQueue();
        EXPECT_EQ(producer->GetFamilyIndex(), consumer->GetFamilyIndex());
        ASSERT_EQ(vkResetCommandPool(device.GetVkHandle(), commands.pool, 0), VK_SUCCESS);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        ASSERT_EQ(vkBeginCommandBuffer(commands.buffers[0], &begin), VK_SUCCESS);
        vkCmdFillBuffer(commands.buffers[0], data.buffer, 0, 4, 0x12345678);
        ASSERT_EQ(vkEndCommandBuffer(commands.buffers[0]), VK_SUCCESS);
        const VkSemaphore signal = semaphore.GetVkHandle();
        VkSubmitInfo producerSubmit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        producerSubmit.commandBufferCount   = 1;
        producerSubmit.pCommandBuffers      = &commands.buffers[0];
        producerSubmit.signalSemaphoreCount = alias ? 0 : 1;
        producerSubmit.pSignalSemaphores    = &signal;
        ASSERT_EQ(vkQueueSubmit(producer->GetVkHandle(), 1, &producerSubmit, VK_NULL_HANDLE),
                  VK_SUCCESS);
        ASSERT_EQ(vkBeginCommandBuffer(commands.buffers[1], &begin), VK_SUCCESS);
        if (alias)
        {
            VulkanPipelineBarrier barrier;
            barrier.AddBufferBarrier(data.buffer, 0, 8, VK_ACCESS_TRANSFER_WRITE_BIT,
                                     VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
            barrier.Execute(commands.buffers[1], VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT);
        }
        const VkBufferCopy copy{0, 4, 4};
        vkCmdCopyBuffer(commands.buffers[1], data.buffer, data.buffer, 1, &copy);
        VulkanPipelineBarrier host;
        host.AddBufferBarrier(data.buffer, 4, 4, VK_ACCESS_TRANSFER_WRITE_BIT,
                              VK_ACCESS_HOST_READ_BIT);
        host.Execute(commands.buffers[1], VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_HOST_BIT);
        TopologySubmitObserver::original = vkQueueSubmit;
        TopologySubmitObserver::waits    = 0;
        test::ScopedVulkanCall<PFN_vkQueueSubmit> observe(vkQueueSubmit,
                                                          &TopologySubmitObserver::Submit);
        ASSERT_EQ(vkEndCommandBuffer(commands.buffers[1]), VK_SUCCESS);
        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo consumerSubmit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        consumerSubmit.commandBufferCount = 1;
        consumerSubmit.pCommandBuffers    = &commands.buffers[1];
        consumerSubmit.waitSemaphoreCount = alias ? 0 : 1;
        consumerSubmit.pWaitSemaphores    = &signal;
        consumerSubmit.pWaitDstStageMask  = &waitStage;
        ASSERT_EQ(vkQueueSubmit(consumer->GetVkHandle(), 1, &consumerSubmit, VK_NULL_HANDLE),
                  VK_SUCCESS);
        EXPECT_EQ(TopologySubmitObserver::waits, alias ? 0u : 1u);
        ASSERT_EQ(vkQueueWaitIdle(consumer->GetVkHandle()), VK_SUCCESS);
        void* mapped = nullptr;
        ASSERT_EQ(vkMapMemory(device.GetVkHandle(), data.memory, 0, 8, 0, &mapped), VK_SUCCESS);
        EXPECT_EQ(static_cast<const uint32_t*>(mapped)[1], 0x12345678u);
        vkUnmapMemory(device.GetVkHandle(), data.memory);
    }
}

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
