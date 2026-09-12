#include "VulkanIntegrationFixture.h"
#include "Graphics/VulkanRHI/VulkanCommandList.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanSynchronization.h"
#include <gtest/gtest.h>
#include <unordered_map>
#include <unordered_set>

namespace
{
using namespace zen;

template <typename T> struct ScopedFunction
{
    T& slot;
    T previous;
    ScopedFunction(T& slot, T replacement) : slot(slot), previous(slot)
    {
        slot = replacement;
    }
    ~ScopedFunction()
    {
        slot = previous;
    }
};

// Keep descriptor validity separate from queue completion. Tests advance individual
// queues explicitly, so CPU recording/submission cannot accidentally count as GPU completion.
struct DescriptorDriver
{
    struct Submission
    {
        VkQueue queue{};
        uint64_t serial{};
    };
    static inline uintptr_t nextHandle;
    static inline VkResult submitResult;
    static inline std::unordered_map<VkQueue, uint64_t> submitted, completed;
    static inline std::unordered_map<VkSemaphore, Submission> semaphores;
    static inline std::unordered_map<VkFence, Submission> fences;
    static inline std::unordered_map<VkDescriptorSet, VkDescriptorPool> owners;
    static inline std::unordered_set<VkDescriptorSet> freedSets;
    static inline std::unordered_set<VkDescriptorPool> destroyedPools;

    static void Reset()
    {
        nextHandle   = 1;
        submitResult = VK_SUCCESS;
        submitted.clear();
        completed.clear();
        semaphores.clear();
        fences.clear();
        owners.clear();
        freedSets.clear();
        destroyedPools.clear();
    }
    static void Complete(uint32_t family)
    {
        VkQueue queue    = reinterpret_cast<VkQueue>(uintptr_t(family + 1));
        completed[queue] = submitted[queue];
    }
    static VKAPI_ATTR VkResult VKAPI_CALL CreateCommandPool(VkDevice,
                                                            const VkCommandPoolCreateInfo*,
                                                            const VkAllocationCallbacks*,
                                                            VkCommandPool* pool)
    {
        *pool = reinterpret_cast<VkCommandPool>(nextHandle++);
        return VK_SUCCESS;
    }
    static VKAPI_ATTR void VKAPI_CALL DestroyCommandPool(VkDevice,
                                                         VkCommandPool,
                                                         const VkAllocationCallbacks*)
    {}

    static VKAPI_ATTR void VKAPI_CALL Properties(VkPhysicalDevice, VkPhysicalDeviceProperties* p)
    {
        *p = {};
    }
    static VKAPI_ATTR void VKAPI_CALL GetQueue(VkDevice, uint32_t family, uint32_t, VkQueue* queue)
    {
        *queue = reinterpret_cast<VkQueue>(uintptr_t(family + 1));
    }
    static VKAPI_ATTR VkResult VKAPI_CALL CreatePool(VkDevice,
                                                     const VkDescriptorPoolCreateInfo*,
                                                     const VkAllocationCallbacks*,
                                                     VkDescriptorPool* pool)
    {
        *pool = reinterpret_cast<VkDescriptorPool>(nextHandle++);
        return VK_SUCCESS;
    }
    static void Invalidate(VkDescriptorPool pool)
    {
        for (const auto& [set, owner] : owners)
        {
            if (owner == pool)
            {
                freedSets.insert(set);
            }
        }
    }
    static VKAPI_ATTR void VKAPI_CALL DestroyPool(VkDevice,
                                                  VkDescriptorPool pool,
                                                  const VkAllocationCallbacks*)
    {
        destroyedPools.insert(pool);
        Invalidate(pool);
    }
    static VKAPI_ATTR VkResult VKAPI_CALL ResetPool(VkDevice,
                                                    VkDescriptorPool pool,
                                                    VkDescriptorPoolResetFlags)
    {
        Invalidate(pool);
        return VK_SUCCESS;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Allocate(VkDevice,
                                                   const VkDescriptorSetAllocateInfo* info,
                                                   VkDescriptorSet* set)
    {
        EXPECT_EQ(info->descriptorSetCount, 1u);
        *set         = reinterpret_cast<VkDescriptorSet>(nextHandle++);
        owners[*set] = info->descriptorPool;
        return VK_SUCCESS;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL CreateSemaphore(VkDevice,
                                                          const VkSemaphoreCreateInfo*,
                                                          const VkAllocationCallbacks*,
                                                          VkSemaphore* semaphore)
    {
        *semaphore = reinterpret_cast<VkSemaphore>(nextHandle++);
        return VK_SUCCESS;
    }
    static VKAPI_ATTR void VKAPI_CALL DestroySemaphore(VkDevice,
                                                       VkSemaphore semaphore,
                                                       const VkAllocationCallbacks*)
    {
        semaphores.erase(semaphore);
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Counter(VkDevice, VkSemaphore semaphore, uint64_t* value)
    {
        *value = completed[semaphores[semaphore].queue];
        return VK_SUCCESS;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL CreateFence(VkDevice,
                                                      const VkFenceCreateInfo*,
                                                      const VkAllocationCallbacks*,
                                                      VkFence* fence)
    {
        *fence         = reinterpret_cast<VkFence>(nextHandle++);
        fences[*fence] = {};
        return VK_SUCCESS;
    }
    static VKAPI_ATTR void VKAPI_CALL DestroyFence(VkDevice,
                                                   VkFence fence,
                                                   const VkAllocationCallbacks*)
    {
        fences.erase(fence);
    }
    static VKAPI_ATTR VkResult VKAPI_CALL FenceStatus(VkDevice, VkFence fence)
    {
        const Submission submission = fences[fence];
        return submission.serial != 0 && completed[submission.queue] >= submission.serial ?
            VK_SUCCESS :
            VK_NOT_READY;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL ResetFences(VkDevice,
                                                      uint32_t count,
                                                      const VkFence* handles)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            fences[handles[i]] = {};
        }
        return VK_SUCCESS;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Submit(VkQueue queue,
                                                 uint32_t count,
                                                 const VkSubmitInfo* infos,
                                                 VkFence fence)
    {
        if (submitResult != VK_SUCCESS)
        {
            return submitResult;
        }
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint64_t serial = ++submitted[queue];
            if (fence != VK_NULL_HANDLE)
            {
                fences[fence] = {queue, serial};
            }
            if (infos[i].pNext != nullptr)
            {
                const auto* timeline =
                    static_cast<const VkTimelineSemaphoreSubmitInfo*>(infos[i].pNext);
                for (uint32_t j = 0; j < infos[i].signalSemaphoreCount; ++j)
                {
                    EXPECT_EQ(timeline->pSignalSemaphoreValues[j], serial);
                    semaphores[infos[i].pSignalSemaphores[j]] = {queue, serial};
                }
            }
        }
        return VK_SUCCESS;
    }
};



class VulkanDescriptorLifetimeTest : public testing::TestWithParam<bool>
{
protected:
    template <typename Callback> void WithDevice(Callback callback)
    {
        test::VulkanSession session;
        VulkanDevice& device = *session.rhi.GetDevice();
        DescriptorDriver::Reset();
        ScopedFunction createCommandPool(vkCreateCommandPool, &DescriptorDriver::CreateCommandPool);
        ScopedFunction destroyCommandPool(vkDestroyCommandPool,
                                          &DescriptorDriver::DestroyCommandPool);
        ScopedFunction getQueue(vkGetDeviceQueue, &DescriptorDriver::GetQueue);
        ScopedFunction createPool(vkCreateDescriptorPool, &DescriptorDriver::CreatePool);
        ScopedFunction destroyPool(vkDestroyDescriptorPool, &DescriptorDriver::DestroyPool);
        ScopedFunction resetPool(vkResetDescriptorPool, &DescriptorDriver::ResetPool);
        ScopedFunction allocate(vkAllocateDescriptorSets, &DescriptorDriver::Allocate);
        ScopedFunction createSemaphore(vkCreateSemaphore, &DescriptorDriver::CreateSemaphore);
        ScopedFunction destroySemaphore(vkDestroySemaphore, &DescriptorDriver::DestroySemaphore);
        ScopedFunction counter(vkGetSemaphoreCounterValue, &DescriptorDriver::Counter);
        ScopedFunction createFence(vkCreateFence, &DescriptorDriver::CreateFence);
        ScopedFunction destroyFence(vkDestroyFence, &DescriptorDriver::DestroyFence);
        ScopedFunction fenceStatus(vkGetFenceStatus, &DescriptorDriver::FenceStatus);
        ScopedFunction resetFences(vkResetFences, &DescriptorDriver::ResetFences);
        ScopedFunction submit(vkQueueSubmit, &DescriptorDriver::Submit);
        device.GetExtensionFlags().hasTimelineSemaphore = GetParam();
        VulkanDescriptorPoolManager2 manager(&device);
        callback(device, manager);
        manager.Destroy();
        // Drain the public fence manager before restoring the intercepted native calls.
        device.GetFenceManager()->Destroy();
        EXPECT_TRUE(DescriptorDriver::fences.empty());
        EXPECT_TRUE(DescriptorDriver::semaphores.empty());
    }

    static VkDescriptorSet Allocate(VulkanDescriptorPoolSetContainer* container)
    {
        VulkanDescriptorPoolKey key{};
        key.descriptorCount[ToUnderlying(RHIShaderResourceType::eStorageBuffer)] = 1;
        return container->Allocate(key, false,
                                   reinterpret_cast<VkDescriptorSetLayout>(uintptr_t(1)), 0);
    }

    static void Enqueue(VulkanCommandContextBase& context, VulkanQueue& queue)
    {
        HeapVector<VulkanWorkload*> workloads;
        context.CollectWorkloads(workloads);
        for (VulkanWorkload* workload : workloads)
        {
            queue.EnqueueWorkload(workload);
        }
    }
};

TEST_P(VulkanDescriptorLifetimeTest, RetiredPoolSurvivesRecordingAndPendingSubmission)
{
    WithDevice([](VulkanDevice& device, VulkanDescriptorPoolManager2& manager) {
        VulkanQueue queue(&device, 0);
        VulkanCommandContextBase context(&queue, VulkanCommandBufferType::ePrimary);
        auto* container       = manager.AcquireDescriptorPoolSetContainer();
        VkDescriptorSet first = Allocate(container);
        context.RetainDescriptorPool(container);
        manager.ReleaseContainer(container);
        manager.BeginFrame(10);
        EXPECT_FALSE(DescriptorDriver::freedSets.contains(first));
        Enqueue(context, queue);
        uint64_t serial = 0;
        ASSERT_EQ(queue.SubmitPendingWorkloads(serial), RHISubmissionResult::eSuccess);
        queue.ProcessPendingWorkloads(0);
        manager.BeginFrame(20);
        EXPECT_EQ(queue.GetLastCompletedSerial(), 0u);
        EXPECT_FALSE(DescriptorDriver::freedSets.contains(first));
        DescriptorDriver::Complete(0);
        queue.ProcessPendingWorkloads(0);
        manager.BeginFrame(21);
        EXPECT_EQ(queue.GetLastCompletedSerial(), serial);
        EXPECT_TRUE(DescriptorDriver::freedSets.contains(first));
    });
}

TEST_P(VulkanDescriptorLifetimeTest, SharedPoolWaitsForBothQueuesAndDeduplicatesRepeatedRetention)
{
    WithDevice([](VulkanDevice& device, VulkanDescriptorPoolManager2& manager) {
        VulkanQueue firstQueue(&device, 0), secondQueue(&device, 1);
        VulkanCommandContextBase firstContext(&firstQueue, VulkanCommandBufferType::ePrimary);
        VulkanCommandContextBase secondContext(&secondQueue, VulkanCommandBufferType::ePrimary);
        auto* container       = manager.AcquireDescriptorPoolSetContainer();
        VkDescriptorSet first = Allocate(container);
        firstContext.RetainDescriptorPool(container);
        firstContext.RetainDescriptorPool(container);
        secondContext.RetainDescriptorPool(container);
        manager.ReleaseContainer(container);
        Enqueue(firstContext, firstQueue);
        Enqueue(secondContext, secondQueue);
        uint64_t serial = 0;
        ASSERT_EQ(firstQueue.SubmitPendingWorkloads(serial), RHISubmissionResult::eSuccess);
        ASSERT_EQ(secondQueue.SubmitPendingWorkloads(serial), RHISubmissionResult::eSuccess);
        DescriptorDriver::Complete(0);
        firstQueue.ProcessPendingWorkloads(0);
        secondQueue.ProcessPendingWorkloads(0);
        manager.BeginFrame(1);
        EXPECT_FALSE(DescriptorDriver::freedSets.contains(first));
        DescriptorDriver::Complete(1);
        secondQueue.ProcessPendingWorkloads(0);
        manager.BeginFrame(2);
        EXPECT_TRUE(DescriptorDriver::freedSets.contains(first));
    });
}

TEST_P(VulkanDescriptorLifetimeTest, BatchCompletionReleasesMergedChildOwnership)
{
    WithDevice([](VulkanDevice& device, VulkanDescriptorPoolManager2& manager) {
        VulkanQueue queue(&device, 0);
        auto* container       = manager.AcquireDescriptorPoolSetContainer();
        VkDescriptorSet first = Allocate(container);
        for (int i = 0; i < 3; ++i)
        {
            VulkanCommandContextBase context(&queue, VulkanCommandBufferType::ePrimary);
            context.RetainDescriptorPool(container);
            Enqueue(context, queue);
        }
        manager.ReleaseContainer(container);
        uint64_t serial = 0;
        ASSERT_EQ(queue.SubmitPendingWorkloads(serial), RHISubmissionResult::eSuccess);
        manager.BeginFrame(1);
        EXPECT_FALSE(DescriptorDriver::freedSets.contains(first));
        DescriptorDriver::Complete(0);
        queue.ProcessPendingWorkloads(0);
        manager.BeginFrame(2);
        EXPECT_TRUE(DescriptorDriver::freedSets.contains(first));
    });
}

TEST_P(VulkanDescriptorLifetimeTest, DefiniteRejectionReleasesPoolOnlyWhenDiscarded)
{
    WithDevice([](VulkanDevice& device, VulkanDescriptorPoolManager2& manager) {
        VulkanQueue queue(&device, 0);
        VulkanCommandContextBase context(&queue, VulkanCommandBufferType::ePrimary);
        auto* container       = manager.AcquireDescriptorPoolSetContainer();
        VkDescriptorSet first = Allocate(container);
        context.RetainDescriptorPool(container);
        manager.ReleaseContainer(container);
        Enqueue(context, queue);
        DescriptorDriver::submitResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        uint64_t serial                = 0;
        EXPECT_EQ(queue.SubmitPendingWorkloads(serial), RHISubmissionResult::eRejected);
        manager.BeginFrame(1);
        EXPECT_FALSE(DescriptorDriver::freedSets.contains(first));
        queue.DiscardPendingWorkloads();
        manager.BeginFrame(2);
        EXPECT_TRUE(DescriptorDriver::freedSets.contains(first));
    });
}

TEST_P(VulkanDescriptorLifetimeTest, UncertainSubmissionKeepsPoolsThroughManagerShutdown)
{
    WithDevice([](VulkanDevice& device, VulkanDescriptorPoolManager2& manager) {
        VkDescriptorPool pool;
        {
            VulkanQueue queue(&device, 0);
            VulkanCommandContextBase context(&queue, VulkanCommandBufferType::ePrimary);
            auto* container       = manager.AcquireDescriptorPoolSetContainer();
            VkDescriptorSet first = Allocate(container);
            pool                  = DescriptorDriver::owners[first];
            context.RetainDescriptorPool(container);
            manager.ReleaseContainer(container);
            Enqueue(context, queue);
            DescriptorDriver::submitResult = VK_ERROR_DEVICE_LOST;
            uint64_t serial                = 0;
            EXPECT_EQ(queue.SubmitPendingWorkloads(serial), RHISubmissionResult::eFatal);
            queue.DiscardPendingWorkloads(true);
            manager.BeginFrame(1);
            manager.Destroy();
            EXPECT_FALSE(DescriptorDriver::freedSets.contains(first));
            EXPECT_FALSE(DescriptorDriver::destroyedPools.contains(pool));
        }
        EXPECT_TRUE(DescriptorDriver::destroyedPools.contains(pool));
    });
}

INSTANTIATE_TEST_SUITE_P(TimelineAndFence, VulkanDescriptorLifetimeTest, testing::Bool());
} // namespace
