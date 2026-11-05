#include "Graphics/RHI/RHICommandListExecutor.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanQueue.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "ScopedVulkanCall.h"
#include <gtest/gtest.h>
#include <array>
#include <cstring>

namespace
{
using namespace zen;

// Gives any GPU the fallback topology. Without a dedicated transfer family, the engine's own
// queue selection shares the compute VkQueue between two logical wrappers. volk resolves
// instance functions during Init, so the family query is replaced through its loader.
struct HiddenTransferFamily
{
    static inline PFN_vkGetInstanceProcAddr                    originalProcAddr{};
    static inline PFN_vkGetPhysicalDeviceQueueFamilyProperties originalFamilies{};

    static VKAPI_ATTR void VKAPI_CALL Families(VkPhysicalDevice gpu, uint32_t* count, VkQueueFamilyProperties* families)
    {
        originalFamilies(gpu, count, families);

        for (uint32_t i = 0; families != nullptr && i < *count; ++i)
        {
            if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == 0)
            {
                families[i].queueCount = 0;
            }
        }
    }

    static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL ProcAddr(VkInstance instance, const char* name)
    {
        PFN_vkVoidFunction function = originalProcAddr(instance, name);

        if (std::strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties") == 0)
        {
            originalFamilies = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(function);

            function         = reinterpret_cast<PFN_vkVoidFunction>(&Families);
        }

        return function;
    }
};

class VulkanAliasedQueueIntegrationTest : public testing::TestWithParam<RHIExecutionMode>
{
protected:
    struct NativeSubmission
    {
        VkQueue     queue;
        VkSemaphore timeline;
    };

    // Each group copies the previous group's output, so a reversed submission leaves stale data.
    struct CopyChain
    {
        RefCountPtr<RHISubmissionState>       state;
        HeapVector<RHICommandListPtr>         lists;
        HeapVector<RHISubmissionGroup>        groups;
        HeapVector<RHIResourcePtr<RHIBuffer>> links;
    };

    static VKAPI_ATTR VkBool32 VKAPI_CALL Validation(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                                     VkDebugUtilsMessageTypeFlagsEXT,
                                                     const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                     void*)
    {
        ADD_FAILURE() << data->pMessage;

        return VK_TRUE;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL Submit(VkQueue queue, uint32_t count, const VkSubmitInfo* infos, VkFence fence)
    {
        ++submitCalls;

        for (uint32_t i = 0; i < count; ++i)
        {
            // The logical wrapper's timeline is the last signal of each submit info.
            submissions.push_back({queue, infos[i].pSignalSemaphores[infos[i].signalSemaphoreCount - 1]});
        }

        return submitCalls == failSubmitAt ? VK_ERROR_OUT_OF_HOST_MEMORY : originalSubmit(queue, count, infos, fence);
    }

    void SetUp() override
    {
        backend                                = ZEN_NEW() VulkanRHI();

        HiddenTransferFamily::originalProcAddr = vkGetInstanceProcAddr;

        {
            test::ScopedVulkanCall<PFN_vkGetInstanceProcAddr> procAddr(vkGetInstanceProcAddr, &HiddenTransferFamily::ProcAddr);

            backend->Init();
        }

        GDynamicRHI = backend;

        VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};

        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;

        info.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;

        info.pfnUserCallback = Validation;

        ASSERT_EQ(vkCreateDebugUtilsMessengerEXT(backend->GetInstance(), &info, nullptr, &messenger), VK_SUCCESS);

        executor    = ZEN_NEW() RHICommandListExecutor(backend, GetParam());

        GDynamicRHI = executor;

        GetRHIThread().Invoke(&RHIFrameState::Init, &GRHIFrameState, 3);

        source   = Buffer(RHIBufferAllocateType::eCPUWrite);

        readback = Buffer(RHIBufferAllocateType::eCPURead);

        for (uint32_t i = 0; i < bytes.size(); ++i)
        {
            bytes[i] = uint8_t(i * 3 + 7);
        }

        uint8_t* mapped = source->Map();

        ASSERT_NE(mapped, nullptr);

        std::memcpy(mapped, bytes.data(), bytes.size());

        source->Unmap();

        originalSubmit = vkQueueSubmit;

        failSubmitAt   = 0;

        submitCalls    = 0;

        submissions.clear();

        const VulkanDevice* device = backend->GetDevice();

        if (!device->SupportsTimelineSemaphore())
        {
            GTEST_SKIP() << "Grouped runs require timeline submissions";
        }

        const VulkanQueue* transfer = device->GetQueue(RHICommandContextType::eTransfer);

        const VulkanQueue* compute  = device->GetQueue(RHICommandContextType::eAsyncCompute);

        ASSERT_NE(transfer, compute);

        ASSERT_EQ(transfer->GetVkHandle(), compute->GetVkHandle());

        ASSERT_TRUE(executor->GetQueueCapabilities().AreQueuesShared(RHICommandContextType::eTransfer,
                                                                     RHICommandContextType::eAsyncCompute));

        LearnTimeline(RHICommandContextType::eTransfer);

        LearnTimeline(RHICommandContextType::eAsyncCompute);

        ASSERT_NE(timelines[ToUnderlying(RHICommandContextType::eTransfer)],
                  timelines[ToUnderlying(RHICommandContextType::eAsyncCompute)]);
    }

    void TearDown() override
    {
        if (executor != nullptr)
        {
            executor->WaitDeviceIdle();

            source->ReleaseReference();

            readback->ReleaseReference();

            vkDestroyDebugUtilsMessengerEXT(backend->GetInstance(), messenger, nullptr);

            executor->Destroy();

            ZEN_DELETE(executor);
        }
        else if (backend != nullptr)
        {
            backend->Destroy();

            ZEN_DELETE(backend);
        }

        GDynamicRHI = nullptr;

        GVulkanRHI  = nullptr;

        // Release the static capture buffer before the allocator's shutdown report.
        submissions = {};
    }

    RHIBuffer* Buffer(RHIBufferAllocateType allocation)
    {
        RHIBufferCreateInfo info{};

        info.size         = bytes.size();

        info.allocateType = allocation;

        info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eTransferSrcBuffer, RHIBufferUsageFlagBits::eTransferDstBuffer);

        return executor->CreateBuffer(info);
    }

    // A native submission on the shared VkQueue reveals its wrapper only through this timeline.
    void LearnTimeline(RHICommandContextType queue)
    {
        RHIResourcePtr<RHIBuffer> scratch(Buffer(RHIBufferAllocateType::eGPU), false);

        RHICommandListPtr commands(RHICommandList::Create(executor->GetCommandContext(queue)));

        commands->CopyBuffer(source, scratch.Get(), {0, 0, bytes.size()});

        RHICommandList* list = commands.get();

        submissions.clear();

        test::ScopedVulkanCall<PFN_vkQueueSubmit> submitHook(vkQueueSubmit, &Submit);

        ASSERT_EQ(executor->SubmitBatch(MakeVecView(&list, 1)), RHISubmissionResult::eSuccess);

        ASSERT_EQ(submissions.size(), 1u);

        timelines[ToUnderlying(queue)] = submissions[0].timeline;

        submissions.clear();

        submitCalls = 0;

        // The scratch buffer leaves scope with this call.
        executor->WaitDeviceIdle();
    }

    void BuildChain(VectorView<const RHICommandContextType> queues, CopyChain& chain)
    {
        chain.state = MakeRefCountPtr<RHISubmissionState>(queues);

        for (uint32_t i = 0; i < queues.size(); ++i)
        {
            RHIBuffer* input = i == 0 ? source : chain.links.back().Get();

            if (i + 1 != queues.size())
            {
                chain.links.emplace_back(Buffer(RHIBufferAllocateType::eGPU), false);
            }

            RHIBuffer* output = i + 1 == queues.size() ? readback : chain.links.back().Get();

            chain.lists.emplace_back(RHICommandList::Create(executor->GetCommandContext(queues[i])));

            RHICommandList& commands = *chain.lists.back();

            if (i != 0)
            {
                // The shared queue orders the groups; only this barrier makes the prior copy visible.
                HeapVector<RHIBufferTransition> buffers;

                buffers.push_back({RHIAccessMode::eReadWrite, RHIAccessMode::eRead, input, RHIBufferUsage::eTransferDst,
                                   RHIBufferUsage::eTransferSrc});

                const BitField<RHIPipelineStageFlagBits> transfer(RHIPipelineStageFlagBits::eTransfer);

                commands.AddTransitions(transfer, transfer, {}, buffers, {});
            }

            commands.CopyBuffer(input, output, {0, 0, bytes.size()});

            chain.groups.push_back({&commands});

            if (i != 0)
            {
                chain.groups.back().predecessors.push_back({queues[i - 1], 0, chain.state, i - 1});
            }
        }
    }

    void ExpectNativeOrder(VectorView<const RHICommandContextType> queues)
    {
        const VkQueue shared = backend->GetDevice()->GetQueue(queues[0])->GetVkHandle();

        ASSERT_EQ(submissions.size(), queues.size());

        for (uint32_t i = 0; i < queues.size(); ++i)
        {
            EXPECT_EQ(submissions[i].queue, shared) << i;

            EXPECT_EQ(submissions[i].timeline, timelines[ToUnderlying(queues[i])]) << i;
        }
    }

    void VerifyChain(VectorView<const RHICommandContextType> queues)
    {
        CopyChain chain;

        BuildChain(queues, chain);

        RHIBatchResult result;

        {
            test::ScopedVulkanCall<PFN_vkQueueSubmit> submitHook(vkQueueSubmit, &Submit);

            result = executor->SubmitGroups(chain.groups, chain.state);
        }

        ASSERT_EQ(result.submission, RHISubmissionResult::eSuccess);

        ExpectNativeOrder(queues);

        executor->WaitDeviceIdle();

        const uint8_t* mapped = readback->Map();

        ASSERT_NE(mapped, nullptr);

        EXPECT_EQ(std::memcmp(mapped, bytes.data(), bytes.size()), 0);

        readback->Unmap();
    }

    VulkanRHI*                                                         backend{nullptr};
    RHICommandListExecutor*                                            executor{nullptr};
    VkDebugUtilsMessengerEXT                                           messenger{VK_NULL_HANDLE};
    RHIBuffer*                                                         source{nullptr};
    RHIBuffer*                                                         readback{nullptr};
    std::array<uint8_t, 64>                                            bytes{};
    std::array<VkSemaphore, ToUnderlying(RHICommandContextType::eMax)> timelines{};
    static inline PFN_vkQueueSubmit                                    originalSubmit{};
    static inline uint32_t                                             failSubmitAt{0};
    static inline uint32_t                                             submitCalls{0};
    static inline HeapVector<NativeSubmission>                         submissions;
};

TEST_P(VulkanAliasedQueueIntegrationTest, TransferProducerPrecedesComputeConsumerOnTheNativeQueue)
{
    const RHICommandContextType queues[] = {RHICommandContextType::eTransfer, RHICommandContextType::eAsyncCompute};

    VerifyChain(MakeVecView(queues, 2));
}

TEST_P(VulkanAliasedQueueIntegrationTest, ComputeTransferComputeKeepsInputOrderOnTheNativeQueue)
{
    const RHICommandContextType queues[] = {RHICommandContextType::eAsyncCompute, RHICommandContextType::eTransfer,
                                            RHICommandContextType::eAsyncCompute};

    VerifyChain(MakeVecView(queues, 3));
}

TEST_P(VulkanAliasedQueueIntegrationTest, RejectedSecondRunKeepsTheAcceptedPrefix)
{
    const RHICommandContextType queues[] = {RHICommandContextType::eTransfer, RHICommandContextType::eAsyncCompute,
                                            RHICommandContextType::eTransfer};

    CopyChain chain;

    BuildChain(MakeVecView(queues, 3), chain);

    const uint64_t transferBefore = executor->GetLastSubmittedSerial(RHICommandContextType::eTransfer);

    const uint64_t computeBefore  = executor->GetLastSubmittedSerial(RHICommandContextType::eAsyncCompute);

    failSubmitAt                  = 2;

    RHIBatchResult result;

    {
        test::ScopedVulkanCall<PFN_vkQueueSubmit> submitHook(vkQueueSubmit, &Submit);

        result = executor->SubmitGroups(chain.groups, chain.state);
    }

    ASSERT_EQ(result.submission, RHISubmissionResult::eFatal);

    ASSERT_EQ(result.groups.size(), 3u);

    EXPECT_EQ(result.groups[0].submission, RHISubmissionResult::eSuccess);

    EXPECT_EQ(result.groups[0].accepted.serial, transferBefore + 1);

    EXPECT_EQ(result.groups[1].submission, RHISubmissionResult::eFatal);

    EXPECT_EQ(result.groups[1].accepted.serial, 0u);

    EXPECT_EQ(result.groups[2].submission, RHISubmissionResult::eRejected);

    EXPECT_EQ(result.groups[2].accepted.serial, 0u);

    // The rejected consumer reached the native call; its later run never did.
    ExpectNativeOrder(MakeVecView(queues, 2));

    EXPECT_EQ(executor->GetLastSubmittedSerial(RHICommandContextType::eTransfer), transferBefore + 1);

    EXPECT_EQ(executor->GetLastSubmittedSerial(RHICommandContextType::eAsyncCompute), computeBefore);

    EXPECT_TRUE(executor->AreSubmissionsBlocked());

    // The accepted copy still uses the chain's buffers.
    executor->WaitDeviceIdle();
}

INSTANTIATE_TEST_SUITE_P(ExecutionModes,
                         VulkanAliasedQueueIntegrationTest,
                         testing::Values(RHIExecutionMode::eInline, RHIExecutionMode::eThreaded));
} // namespace
