#include "VulkanIntegrationFixture.h"
#include "ScopedVulkanCall.h"
#include "Graphics/VulkanRHI/VulkanCommandList.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanSynchronization.h"
#include <gtest/gtest.h>
#include <algorithm>

namespace
{
using namespace zen;

struct CommandBufferAllocations
{
    static inline PFN_vkAllocateCommandBuffers allocate;
    static inline PFN_vkFreeCommandBuffers free;
    static inline uint32_t live;
    static inline uint32_t peak;

    static VKAPI_ATTR VkResult VKAPI_CALL Allocate(VkDevice device,
                                                   const VkCommandBufferAllocateInfo* info,
                                                   VkCommandBuffer* buffers)
    {
        const VkResult result = allocate(device, info, buffers);
        if (result == VK_SUCCESS)
        {
            live += info->commandBufferCount;
            peak = std::max(peak, live);
        }
        return result;
    }

    static VKAPI_ATTR void VKAPI_CALL Free(VkDevice device,
                                           VkCommandPool pool,
                                           uint32_t count,
                                           const VkCommandBuffer* buffers)
    {
        free(device, pool, count, buffers);
        live -= count;
    }
};

enum class AbandonedRecordingState
{
    eRecording,
    eRendering,
    eEnded,
    eFinalizedAndRecording,
};

class VulkanCommandBufferAbandonmentTest : public testing::TestWithParam<AbandonedRecordingState>
{
protected:
    void CheckBoundedAllocations(bool reuseCompletedBuffer)
    {
        test::VulkanSession session;
        VulkanDevice* device = session.rhi.GetDevice();
        VulkanSemaphore signal(device);
        CommandBufferAllocations::allocate = vkAllocateCommandBuffers;
        CommandBufferAllocations::free     = vkFreeCommandBuffers;
        CommandBufferAllocations::live     = 0;
        CommandBufferAllocations::peak     = 0;
        test::ScopedVulkanCall<PFN_vkAllocateCommandBuffers> allocate(
            vkAllocateCommandBuffers, CommandBufferAllocations::Allocate);
        test::ScopedVulkanCall<PFN_vkFreeCommandBuffers> free(vkFreeCommandBuffers,
                                                              CommandBufferAllocations::Free);

        if (reuseCompletedBuffer)
        {
            // A recently completed buffer stays within the trim cooldown, exercising reset/reuse.
            FVulkanCommandListContext context(RHICommandContextType::eGraphics, device);
            context.GetCommandBuffer();
            ASSERT_EQ(context.SubmitRecordedWorkloads(), RHISubmissionResult::eSuccess);
            ASSERT_TRUE(context.GetQueue()->WaitForCompletion(context.GetLastSubmittedSerial(),
                                                              UINT64_MAX));
        }

        const uint32_t buffersPerContext =
            GetParam() == AbandonedRecordingState::eFinalizedAndRecording ? 2u : 1u;
        FVulkanCommandBufferPool* pool = nullptr;
        for (uint32_t attempt = 0; attempt < 8; ++attempt)
        {
            SCOPED_TRACE(attempt);
            HeapVector<FVulkanCommandBuffer*> recordings;
            {
                FVulkanCommandListContext context(RHICommandContextType::eGraphics, device);
                FVulkanCommandBuffer* buffer = context.GetCommandBuffer();
                recordings.push_back(buffer);
                if (pool != nullptr)
                {
                    EXPECT_EQ(buffer->GetCommandBufferPool(), pool);
                }
                pool = buffer->GetCommandBufferPool();
                switch (GetParam())
                {
                    case AbandonedRecordingState::eRecording: break;
                    case AbandonedRecordingState::eRendering:
                    {
                        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
                        rendering.renderArea.extent = {1, 1};
                        rendering.layerCount        = 1;
                        buffer->BeginRendering(&rendering);
                        break;
                    }
                    case AbandonedRecordingState::eEnded: buffer->End(); break;
                    case AbandonedRecordingState::eFinalizedAndRecording:
                    {
                        context.AddSignalSemaphore(&signal);
                        // Returning to execution finalizes the preceding workload.
                        recordings.push_back(context.GetCommandBuffer());
                        break;
                    }
                }
            }
            for (FVulkanCommandBuffer* recording : recordings)
            {
                // Wrappers remain pool-owned even when their native allocation is trimmed.
                EXPECT_FALSE(recording->HasBegun());
                EXPECT_FALSE(recording->HasEnded());
            }
            pool->FreeUnusedCommandBuffers();
        }
        // Both retaining reusable buffers and freeing/reallocating them are valid.
        EXPECT_LE(CommandBufferAllocations::peak, buffersPerContext);
        EXPECT_LE(CommandBufferAllocations::live, buffersPerContext);
    }
};

TEST_P(VulkanCommandBufferAbandonmentTest, DestructionMakesNewRecordingsReclaimable)
{
    CheckBoundedAllocations(false);
}

TEST_P(VulkanCommandBufferAbandonmentTest, DestructionMakesPreviouslySubmittedBuffersReusable)
{
    CheckBoundedAllocations(true);
}

INSTANTIATE_TEST_SUITE_P(RecordingStates,
                         VulkanCommandBufferAbandonmentTest,
                         testing::Values(AbandonedRecordingState::eRecording,
                                         AbandonedRecordingState::eRendering,
                                         AbandonedRecordingState::eEnded,
                                         AbandonedRecordingState::eFinalizedAndRecording));

class VulkanCommandBufferOwnershipTest : public testing::TestWithParam<bool>
{};

TEST_P(VulkanCommandBufferOwnershipTest, ContextDestructionPreservesTransferredWork)
{
    test::VulkanSession session;
    VulkanDevice* device              = session.rhi.GetDevice();
    VulkanQueue* queue                = device->GetGfxQueue();
    FVulkanCommandBuffer* transferred = nullptr;
    VkCommandBuffer handle            = VK_NULL_HANDLE;
    uint64_t serial                   = 0;
    {
        FVulkanCommandListContext context(RHICommandContextType::eGraphics, device);
        transferred = context.GetCommandBuffer();
        handle      = transferred->GetVkHandle();
        HeapVector<VulkanWorkload*> workloads;
        context.CollectWorkloads(workloads);
        for (VulkanWorkload* workload : workloads)
        {
            queue->EnqueueWorkload(workload);
        }
        if (GetParam())
        {
            ASSERT_EQ(queue->SubmitPendingWorkloads(serial), RHISubmissionResult::eSuccess);
        }
        // Only this new recording is abandoned; the queue owns the earlier one.
        FVulkanCommandBuffer* abandoned = context.GetCommandBuffer();
        EXPECT_EQ(abandoned->GetCommandBufferPool(), transferred->GetCommandBufferPool());
        EXPECT_NE(abandoned->GetVkHandle(), handle);
    }

    ASSERT_TRUE(transferred->IsAllocated());
    ASSERT_EQ(transferred->GetVkHandle(), handle);
    ASSERT_TRUE(GetParam() ? transferred->IsSubmitted() : transferred->HasEnded());
    {
        FVulkanCommandListContext nextContext(RHICommandContextType::eGraphics, device);
        FVulkanCommandBuffer* next = nextContext.GetCommandBuffer();
        EXPECT_EQ(next->GetCommandBufferPool(), transferred->GetCommandBufferPool());
        EXPECT_NE(next->GetVkHandle(), handle);
    }
    if (!GetParam())
    {
        ASSERT_EQ(queue->SubmitPendingWorkloads(serial), RHISubmissionResult::eSuccess);
    }
    EXPECT_TRUE(queue->WaitForCompletion(serial, UINT64_MAX));
}

INSTANTIATE_TEST_SUITE_P(QueuedOrSubmitted, VulkanCommandBufferOwnershipTest, testing::Bool());
} // namespace
