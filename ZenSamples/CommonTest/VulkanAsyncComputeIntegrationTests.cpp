#include "VulkanIntegrationFixture.h"
#include "ScopedVulkanCall.h"
#include "Graphics/VulkanRHI/VulkanBuffer.h"
#include "Graphics/VulkanRHI/VulkanCommandList.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanExtension.h"
#include "Graphics/VulkanRHI/VulkanMemory.h"
#include "Graphics/VulkanRHI/VulkanPipeline.h"
#include "Graphics/VulkanRHI/VulkanSynchronization.h"
#include "Graphics/VulkanRHI/VulkanTexture.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>

namespace
{
using namespace zen;

class VulkanAsyncComputeIntegrationTest : public testing::TestWithParam<bool>
{
protected:
    std::unique_ptr<test::VulkanSession> session;
    VkDebugUtilsMessengerEXT messenger{};
    HeapVector<FVulkanCommandListContext*> contexts;
    HeapVector<VulkanBuffer*> buffers;
    HeapVector<VulkanTexture*> textures;
    HeapVector<VulkanSemaphore*> semaphores;
    RHIShader* shader{};
    RHIPipeline* pipeline{};
    std::unique_ptr<VulkanMemoryAllocator> observedAllocator;
    VulkanMemoryAllocator* originalAllocator{};
    static inline PFN_vkCreateBuffer createBuffer;
    static inline PFN_vkCreateImage createImage;
    static inline uint32_t bufferCreateCalls;
    static inline uint32_t imageCreateCalls;

    template <typename Info> static void CheckSharing(const Info& info, bool transfer)
    {
        const VulkanDevice* device = GVulkanRHI->GetDevice();
        const uint32_t families[]  = {device->GetGfxQueue()->GetFamilyIndex(),
                                      device->GetComputeQueue()->GetFamilyIndex(),
                                      device->GetTransferQueue()->GetFamilyIndex()};
        EXPECT_EQ(info.sharingMode, VK_SHARING_MODE_CONCURRENT);
        ASSERT_EQ(info.queueFamilyIndexCount, transfer && families[2] != families[0] ? 3u : 2u);
        ASSERT_NE(info.pQueueFamilyIndices, nullptr);
        for (uint32_t index = 0; index < info.queueFamilyIndexCount; ++index)
        {
            EXPECT_NE(std::find(info.pQueueFamilyIndices,
                                info.pQueueFamilyIndices + info.queueFamilyIndexCount,
                                families[index]),
                      info.pQueueFamilyIndices + info.queueFamilyIndexCount);
        }
    }

    static VKAPI_ATTR VkBool32 VKAPI_CALL
    Validation(VkDebugUtilsMessageSeverityFlagBitsEXT,
               VkDebugUtilsMessageTypeFlagsEXT,
               const VkDebugUtilsMessengerCallbackDataEXT* data,
               void*)
    {
        ADD_FAILURE() << data->pMessage;
        // Abort invalid native calls so a regression cannot submit an unsafe dependency.
        return VK_TRUE;
    }

    void SetUp() override
    {
        session              = std::make_unique<test::VulkanSession>();
        VulkanDevice* device = session->rhi.GetDevice();
        if (GetParam())
        {
            ASSERT_TRUE(device->SupportsTimelineSemaphore());
        }
        else
        {
            device->GetExtensionFlags().hasTimelineSemaphore = 0;
        }
        VkDebugUtilsMessengerCreateInfoEXT info{
            VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        info.pfnUserCallback = Validation;
        ASSERT_EQ(
            vkCreateDebugUtilsMessengerEXT(session->rhi.GetInstance(), &info, nullptr, &messenger),
            VK_SUCCESS);
        const uint32_t graphics = device->GetGfxQueue()->GetFamilyIndex();
        const uint32_t compute  = device->GetComputeQueue()->GetFamilyIndex();
        const uint32_t transfer = device->GetTransferQueue()->GetFamilyIndex();
        if (compute == graphics || compute == transfer)
        {
            GTEST_SKIP() << "Requires a compute family distinct from graphics and transfer";
        }
        std::printf("Async sharing: graphics=%u compute=%u transfer=%u, completion=%s\n", graphics,
                    compute, transfer, GetParam() ? "timeline" : "fence");
    }

    void TearDown() override
    {
        if (!session)
        {
            return;
        }
        session->rhi.WaitDeviceIdle();
        for (FVulkanCommandListContext* context : contexts)
        {
            ZEN_DELETE(context);
        }
        if (pipeline)
        {
            session->rhi.DestroyPipeline(pipeline);
        }
        if (shader)
        {
            session->rhi.DestroyShader(shader);
        }
        for (VulkanTexture* texture : textures)
        {
            session->rhi.DestroyTexture(texture);
        }
        for (VulkanBuffer* buffer : buffers)
        {
            session->rhi.DestroyBuffer(buffer);
        }
        for (VulkanSemaphore*& semaphore : semaphores)
        {
            session->rhi.GetDevice()->GetSemaphoreManager()->DestroySemaphore(semaphore);
        }
        if (originalAllocator)
        {
            // All resources created under observation have been destroyed above.
            GVkMemAllocator = originalAllocator;
            observedAllocator.reset();
        }
        vkDestroyDebugUtilsMessengerEXT(session->rhi.GetInstance(), messenger, nullptr);
        session.reset();
    }

    FVulkanCommandListContext* Context(RHICommandContextType type)
    {
        FVulkanCommandListContext* context =
            static_cast<FVulkanCommandListContext*>(session->rhi.GetCommandContext(type));
        contexts.push_back(context);
        return context;
    }

    VulkanBuffer* Buffer(RHIBufferUsageFlagBits usage, bool transfer = false)
    {
        RHIBufferCreateInfo info{};
        info.size         = 8;
        info.allocateType = RHIBufferAllocateType::eCPURead;
        info.usageFlags.SetFlag(usage);
        if (transfer)
        {
            info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);
        }
        VulkanBuffer* buffer = static_cast<VulkanBuffer*>(session->rhi.CreateBuffer(info));
        buffers.push_back(buffer);
        return buffer;
    }

    VulkanTexture* Texture(bool transfer)
    {
        RHITextureCreateInfo info{};
        info.type   = RHITextureType::e2D;
        info.format = DataFormat::eR32UInt;
        info.usageFlags.SetFlag(RHITextureUsageFlagBits::eStorage);
        if (transfer)
        {
            info.usageFlags.SetFlag(RHITextureUsageFlagBits::eTransferDst);
        }
        VulkanTexture* texture = static_cast<VulkanTexture*>(session->rhi.CreateTexture(info));
        textures.push_back(texture);
        return texture;
    }

    void InitializeLayout(FVulkanCommandListContext* context,
                          VulkanTexture* texture,
                          VkPipelineStageFlags dstStage,
                          VkAccessFlags dstAccess)
    {
        VulkanPipelineBarrier barrier;
        barrier.AddImageBarrier(texture->GetVkImage(), VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_GENERAL, texture->GetVkSubresourceRange(), 0,
                                dstAccess);
        barrier.Execute(context->GetCommandBuffer()->GetVkHandle(),
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, dstStage);
        session->rhi.UpdateImageLayout(texture->GetVkImage(), VK_IMAGE_LAYOUT_GENERAL);
    }

    bool Submit(FVulkanCommandListContext* context)
    {
        HeapVector<VulkanWorkload*> workloads;
        context->CollectWorkloads(workloads);
        VulkanQueue* queue = context->GetQueue();
        for (VulkanWorkload* workload : workloads)
        {
            queue->EnqueueWorkload(workload);
        }
        uint64_t serial                  = 0;
        const RHISubmissionResult result = queue->SubmitPendingWorkloads(serial);
        EXPECT_EQ(result, RHISubmissionResult::eSuccess);
        if (result != RHISubmissionResult::eSuccess)
        {
            // Validation rejects invalid submissions before the driver executes them.
            queue->DiscardPendingWorkloads();
            return false;
        }
        EXPECT_GT(serial, 0u);
        return serial != 0;
    }

    bool Handoff(FVulkanCommandListContext* source, FVulkanCommandListContext* destination)
    {
        VulkanSemaphore* semaphore =
            session->rhi.GetDevice()->GetSemaphoreManager()->GetOrCreateSemaphore();
        semaphores.push_back(semaphore);
        source->AddSignalSemaphore(semaphore);
        if (!Submit(source))
        {
            return false;
        }
        // No host wait: the existing backend semaphore path orders the GPU accesses.
        destination->AddWaitSemaphore(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, semaphore);
        return true;
    }

    void Dispatch(FVulkanCommandListContext* context,
                  VulkanBuffer* data,
                  VulkanTexture* texture,
                  VulkanBuffer* output,
                  bool initialize)
    {
        if (pipeline == nullptr)
        {
            RHIShaderCreateInfo info{};
            info.stageFlags.SetFlag(RHIShaderStageFlagBits::eCompute);
            info.spirvFileName[ToUnderlying(RHIShaderStage::eCompute)] =
                std::filesystem::relative(std::filesystem::path(RDG_REFLECTION_TEST_PATH) /
                                              "async_resources.comp.spv",
                                          SPV_SHADER_PATH)
                    .generic_string();
            shader   = session->rhi.CreateShader(info);
            pipeline = session->rhi.CreatePipeline(RHIComputePipelineCreateInfo{shader});
        }
        context->RHIBindPipeline(pipeline);
        RHIBatchedShaderParameters parameters;
        parameters.AddResourceParam(*shader->GetSRDByLocation(test::kLocalResourceSet, 0), data,
                                    nullptr, 0);
        parameters.AddResourceParam(*shader->GetSRDByLocation(test::kLocalResourceSet, 1),
                                    texture->GetDefaultView(), nullptr, 0);
        parameters.AddResourceParam(*shader->GetSRDByLocation(test::kLocalResourceSet, 2), output,
                                    nullptr, 0);
        context->RHISetShaderParameters(parameters);
        const uint32_t mode = initialize ? 1 : 0;
        context->RHISetPushConstants(
            pipeline, MakeVecView(reinterpret_cast<const uint8_t*>(&mode), sizeof(mode)));
        context->RHIDispatch(1, 1, 1);
    }

    void Readback(FVulkanCommandListContext* context,
                  VulkanBuffer* output,
                  uint32_t expectedBuffer,
                  uint32_t expectedImage,
                  VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                  VkAccessFlags srcAccess       = VK_ACCESS_SHADER_WRITE_BIT)
    {
        VulkanPipelineBarrier barrier;
        barrier.AddMemoryBarrier(srcAccess, VK_ACCESS_HOST_READ_BIT);
        barrier.Execute(context->GetCommandBuffer()->GetVkHandle(), srcStage,
                        VK_PIPELINE_STAGE_HOST_BIT);
        ASSERT_TRUE(Submit(context));
        VulkanQueue* queue = context->GetQueue();
        ASSERT_TRUE(queue->WaitForSubmission(queue->GetLastSubmittedSerial(), UINT64_MAX));
        const uint32_t* values = reinterpret_cast<const uint32_t*>(output->Map());
        EXPECT_EQ(values[0], expectedBuffer);
        EXPECT_EQ(values[1], expectedImage);
        output->Unmap();
    }
};

TEST_P(VulkanAsyncComputeIntegrationTest, TransferBufferCanBeClearedOnSeparateComputeQueue)
{
    VulkanBuffer* buffer = Buffer(RHIBufferUsageFlagBits::eTransferDstBuffer);
    uint32_t* values     = reinterpret_cast<uint32_t*>(buffer->Map());
    values[0] = values[1] = 0xdeadbeef;
    buffer->Unmap();
    FVulkanCommandListContext* compute = Context(RHICommandContextType::eAsyncCompute);
    compute->RHIClearBuffer(buffer, 0, 8);
    Readback(compute, buffer, 0, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
}

TEST_P(VulkanAsyncComputeIntegrationTest, NativeAllocationsIncludeAllPermittedFamilies)
{
    createBuffer      = vkCreateBuffer;
    createImage       = vkCreateImage;
    bufferCreateCalls = imageCreateCalls = 0;
    test::ScopedVulkanCall<PFN_vkCreateBuffer> observeBuffer(
        vkCreateBuffer,
        +[](VkDevice device, const VkBufferCreateInfo* info, const VkAllocationCallbacks* allocator,
            VkBuffer* buffer) -> VkResult {
            ++bufferCreateCalls;
            CheckSharing(*info, (info->usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) != 0);
            return createBuffer(device, info, allocator, buffer);
        });
    test::ScopedVulkanCall<PFN_vkCreateImage> observeImage(
        vkCreateImage,
        +[](VkDevice device, const VkImageCreateInfo* info, const VkAllocationCallbacks* allocator,
            VkImage* image) -> VkResult {
            ++imageCreateCalls;
            CheckSharing(*info, (info->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0);
            return createImage(device, info, allocator, image);
        });
    // VMA caches the entry points at initialization. Use a fresh public allocator so
    // RHI CreateBuffer/CreateTexture reach these forwarding observers, then restore
    // the session allocator in TearDown after freeing the observed resources.
    observedAllocator = std::make_unique<VulkanMemoryAllocator>();
    observedAllocator->Init(
        session->rhi.GetInstance(), session->rhi.GetPhysicalDevice(), session->rhi.GetVkDevice(),
        session->rhi.GetDevice()->GetExtensionFlags().hasBufferDeviceAddress != 0);
    originalAllocator = GVkMemAllocator;
    GVkMemAllocator   = observedAllocator.get();
    for (bool transfer : {false, true})
    {
        Buffer(RHIBufferUsageFlagBits::eStorageBuffer, transfer);
        Texture(transfer);
    }
    EXPECT_GE(bufferCreateCalls, 2u);
    EXPECT_GE(imageCreateCalls, 2u);
}

TEST_P(VulkanAsyncComputeIntegrationTest, StorageOnlyResourcesSurviveGraphicsComputeRoundTrip)
{
    VulkanBuffer* data                  = Buffer(RHIBufferUsageFlagBits::eStorageBuffer);
    VulkanTexture* texture              = Texture(false);
    VulkanBuffer* output                = Buffer(RHIBufferUsageFlagBits::eStorageBuffer);
    FVulkanCommandListContext* graphics = Context(RHICommandContextType::eGraphics);
    FVulkanCommandListContext* compute  = Context(RHICommandContextType::eAsyncCompute);
    InitializeLayout(graphics, texture, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    Dispatch(graphics, data, texture, output, true);
    ASSERT_TRUE(Handoff(graphics, compute));
    Dispatch(compute, data, texture, output, false);
    ASSERT_TRUE(Handoff(compute, graphics));
    Dispatch(graphics, data, texture, output, false);
    Readback(graphics, output, 21, 27);
}

TEST_P(VulkanAsyncComputeIntegrationTest, TransferResourcesSurviveTransferComputeGraphicsHandoff)
{
    VulkanBuffer* data                  = Buffer(RHIBufferUsageFlagBits::eStorageBuffer, true);
    VulkanTexture* texture              = Texture(true);
    VulkanBuffer* output                = Buffer(RHIBufferUsageFlagBits::eStorageBuffer);
    FVulkanCommandListContext* transfer = Context(RHICommandContextType::eTransfer);
    FVulkanCommandListContext* compute  = Context(RHICommandContextType::eAsyncCompute);
    FVulkanCommandListContext* graphics = Context(RHICommandContextType::eGraphics);
    InitializeLayout(transfer, texture, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_WRITE_BIT);
    transfer->RHIClearBuffer(data, 0, 8);
    VulkanBuffer* upload = Buffer(RHIBufferUsageFlagBits::eTransferSrcBuffer);
    *reinterpret_cast<uint32_t*>(upload->Map()) = 40;
    upload->Unmap();
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent      = {1, 1, 1};
    vkCmdCopyBufferToImage(transfer->GetCommandBuffer()->GetVkHandle(), upload->GetVkBuffer(),
                           texture->GetVkImage(), VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    ASSERT_TRUE(Handoff(transfer, compute));
    Dispatch(compute, data, texture, output, false);
    ASSERT_TRUE(Handoff(compute, graphics));
    Dispatch(graphics, data, texture, output, false);
    Readback(graphics, output, 10, 54);
}

INSTANTIATE_TEST_SUITE_P(TimelineAndFence, VulkanAsyncComputeIntegrationTest, testing::Bool());
} // namespace
