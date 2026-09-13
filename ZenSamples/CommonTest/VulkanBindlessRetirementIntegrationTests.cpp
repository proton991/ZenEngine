#include "VulkanIntegrationFixture.h"
#include "ScopedVulkanCall.h"
#include "Graphics/VulkanRHI/VulkanBuffer.h"
#include "Graphics/VulkanRHI/VulkanCommandList.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanPipeline.h"
#include "Graphics/VulkanRHI/VulkanTexture.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <memory>

namespace
{
using namespace zen;

class VulkanBindlessRetirementIntegrationTest : public testing::Test
{
protected:
    std::unique_ptr<test::VulkanSession> session;
    FVulkanCommandListContext* context{};
    RHICommandList* commandList{};
    VkDebugUtilsMessengerEXT messenger{};
    HeapVector<RHIBuffer*> buffers;
    HeapVector<RHITexture*> textures;
    HeapVector<RHISampler*> samplers;
    HeapVector<RHIShader*> shaders;
    HeapVector<RHIPipeline*> pipelines;

    static VKAPI_ATTR VkBool32 VKAPI_CALL
    Validation(VkDebugUtilsMessageSeverityFlagBitsEXT,
               VkDebugUtilsMessageTypeFlagsEXT,
               const VkDebugUtilsMessengerCallbackDataEXT* data,
               void*)
    {
        ADD_FAILURE() << data->pMessage;
        return VK_FALSE;
    }

    void SetUp() override
    {
        session = std::make_unique<test::VulkanSession>();
        VkDebugUtilsMessengerCreateInfoEXT info{
            VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        info.pfnUserCallback = Validation;
        ASSERT_EQ(
            vkCreateDebugUtilsMessengerEXT(session->rhi.GetInstance(), &info, nullptr, &messenger),
            VK_SUCCESS);
        context = static_cast<FVulkanCommandListContext*>(
            session->rhi.GetCommandContext(RHICommandContextType::eGraphics));
    }

    void Shutdown()
    {
        if (!session)
        {
            return;
        }
        session->rhi.WaitDeviceIdle();
        if (commandList != nullptr)
        {
            // RHICommandList::Create transfers ownership of its context to the list.
            ZEN_DELETE(commandList);
            commandList = nullptr;
        }
        else
        {
            ZEN_DELETE(context);
        }
        context = nullptr;
        for (auto* pipeline : pipelines)
        {
            session->rhi.DestroyPipeline(pipeline);
        }
        for (auto* shader : shaders)
        {
            session->rhi.DestroyShader(shader);
        }
        for (auto* sampler : samplers)
        {
            if (sampler)
            {
                session->rhi.DestroySampler(sampler);
            }
        }
        for (auto* texture : textures)
        {
            if (texture)
            {
                session->rhi.DestroyTexture(texture);
            }
        }
        for (auto* buffer : buffers)
        {
            session->rhi.DestroyBuffer(buffer);
        }
        vkDestroyDebugUtilsMessengerEXT(session->rhi.GetInstance(), messenger, nullptr);
        session.reset();
    }

    void TearDown() override
    {
        Shutdown();
    }

    VulkanBuffer* Buffer(uint32_t size = 16, bool uniform = false)
    {
        RHIBufferCreateInfo info{};
        info.size         = size;
        info.allocateType = RHIBufferAllocateType::eCPURead;
        info.usageFlags.SetFlag(uniform ? RHIBufferUsageFlagBits::eUniformBuffer :
                                          RHIBufferUsageFlagBits::eStorageBuffer);
        info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);
        auto* buffer = static_cast<VulkanBuffer*>(session->rhi.CreateBuffer(info));
        buffers.push_back(buffer);
        return buffer;
    }

    VulkanTexture* Texture(RHITextureUsageFlagBits usage = RHITextureUsageFlagBits::eSampled)
    {
        RHITextureCreateInfo info{};
        info.type   = RHITextureType::e2D;
        info.format = DataFormat::eR8G8B8A8UNORM;
        info.width = info.height = 4;
        info.usageFlags.SetFlags(usage, RHITextureUsageFlagBits::eTransferSrc,
                                 RHITextureUsageFlagBits::eTransferDst);
        auto* texture = static_cast<VulkanTexture*>(session->rhi.CreateTexture(info));
        textures.push_back(texture);
        return texture;
    }

    RHISampler* Sampler()
    {
        auto* sampler = session->rhi.CreateSampler(RHISamplerCreateInfo{});
        samplers.push_back(sampler);
        return sampler;
    }

    VulkanShader* Shader(const char* file, bool graphics = false)
    {
        RHIShaderCreateInfo info{};
        auto stage = [&](RHIShaderStage stage, const char* name) {
            info.stageFlags.SetFlag(RHIShaderStageToFlagBits(stage));
            info.spirvFileName[ToUnderlying(stage)] =
                std::filesystem::relative(std::filesystem::path(RDG_REFLECTION_TEST_PATH) / name,
                                          SPV_SHADER_PATH)
                    .generic_string();
        };
        if (graphics)
        {
            stage(RHIShaderStage::eVertex, "pipeline.vert.spv");
        }
        stage(graphics ? RHIShaderStage::eFragment : RHIShaderStage::eCompute, file);
        auto* shader = static_cast<VulkanShader*>(session->rhi.CreateShader(info));
        shaders.push_back(shader);
        return shader;
    }

    RHIPipeline* Compute(const char* file)
    {
        auto* pipeline = session->rhi.CreatePipeline(RHIComputePipelineCreateInfo{Shader(file)});
        pipelines.push_back(pipeline);
        context->RHIBindPipeline(pipeline);
        return pipeline;
    }

    VkCommandBuffer Commands()
    {
        return context->GetCommandBuffer()->GetVkHandle();
    }

    void Transition(VulkanTexture* texture,
                    VkImageLayout before,
                    VkImageLayout after,
                    VkAccessFlags src,
                    VkAccessFlags dst,
                    VkPipelineStageFlags srcStage,
                    VkPipelineStageFlags dstStage)
    {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask       = src;
        barrier.dstAccessMask       = dst;
        barrier.oldLayout           = before;
        barrier.newLayout           = after;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image                                             = texture->GetVkImage();
        barrier.subresourceRange = texture->GetVkSubresourceRange();
        vkCmdPipelineBarrier(Commands(), srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);
        session->rhi.UpdateImageLayout(texture->GetVkImage(), after);
    }

    void Initialize(VulkanTexture* texture, bool green = false)
    {
        Transition(texture, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);
        const VkClearColorValue red{{green ? 0.0f : 1.0f, green ? 1.0f : 0.0f, 0, 1}};
        const auto range = texture->GetVkSubresourceRange();
        vkCmdClearColorImage(Commands(), texture->GetVkImage(),
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &red, 1, &range);
        Transition(texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }

    void Enqueue(FVulkanCommandListContext* source)
    {
        HeapVector<VulkanWorkload*> workloads;
        source->CollectWorkloads(workloads);
        for (auto* workload : workloads)
        {
            source->GetQueue()->EnqueueWorkload(workload);
        }
    }

    void CheckPixel(VulkanBuffer* output, uint32_t expected)
    {
        EXPECT_EQ(*reinterpret_cast<uint32_t*>(output->Map()), expected);
        output->Unmap();
    }

    void SubmitAndWait(VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VkAccessFlags access       = VK_ACCESS_SHADER_WRITE_BIT)
    {
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = access;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(Commands(), stage, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0,
                             nullptr, 0, nullptr);
        HeapVector<VulkanWorkload*> workloads;
        context->CollectWorkloads(workloads);
        auto* queue = session->rhi.GetDevice()->GetGfxQueue();
        for (auto* workload : workloads)
        {
            queue->EnqueueWorkload(workload);
        }
        uint64_t serial = 0;
        ASSERT_EQ(queue->SubmitPendingWorkloads(serial), RHISubmissionResult::eSuccess);
        ASSERT_TRUE(queue->WaitForSubmission(serial, UINT64_MAX));
    }

    void PushIndex(RHIPipeline* pipeline, uint32_t index)
    {
        context->RHISetPushConstants(
            pipeline, MakeVecView(reinterpret_cast<const uint8_t*>(&index), sizeof(index)));
    }

    void SetOutput(RHIPipeline* pipeline, RHIBuffer* output, uint32_t set, uint32_t binding)
    {
        RHIBatchedShaderParameters parameters;
        parameters.AddResourceParam(*pipeline->GetShader()->GetSRDByLocation(set, binding), output,
                                    nullptr, 0);
        context->RHISetShaderParameters(parameters);
    }
};


TEST_F(VulkanBindlessRetirementIntegrationTest, HandlesRejectStaleRetirementAfterSlotReuse)
{
    RHISampler* first        = Sampler();
    RHISampler* replacement  = Sampler();
    RHIBindlessHandle handle = session->rhi.RegisterBindlessResource(first);
    ASSERT_TRUE(handle.IsValid());
    EXPECT_EQ(handle.heapType, RHIBindlessHeapType::eSampler);
    EXPECT_EQ(first->GetRefCount(), 2u);
    RHIBindlessHandle repeated = session->rhi.RegisterBindlessResource(first, handle.slotIndex);
    EXPECT_EQ(repeated.generation, handle.generation);
    ASSERT_TRUE(session->rhi.UnregisterBindlessResource(handle));
    EXPECT_FALSE(session->rhi.IsBindlessResourceRegistered(handle));
    EXPECT_EQ(first->GetRefCount(), 1u);
    RHIBindlessHandle next = session->rhi.RegisterBindlessResource(replacement);
    ASSERT_TRUE(next.IsValid());
    EXPECT_EQ(next.slotIndex, handle.slotIndex);
    EXPECT_NE(next.generation, handle.generation);
    EXPECT_FALSE(session->rhi.UnregisterBindlessResource(handle));
    EXPECT_TRUE(session->rhi.IsBindlessResourceRegistered(next));
    EXPECT_FALSE(session->rhi.UnregisterBindlessResource({}));
    RHIBindlessHandle invalid = next;
    invalid.slotIndex = GetBindlessHeapCapacity(next.heapType);
    EXPECT_FALSE(session->rhi.UnregisterBindlessResource(invalid));
    invalid          = next;
    invalid.heapType = RHIBindlessHeapType::eMax;
    EXPECT_FALSE(session->rhi.IsBindlessResourceRegistered(invalid));
    EXPECT_TRUE(session->rhi.UnregisterBindlessResource(next));
    EXPECT_FALSE(session->rhi.IsBindlessResourceRegistered(next));
    EXPECT_EQ(replacement->GetRefCount(), 1u);
}

TEST_F(VulkanBindlessRetirementIntegrationTest,
       AutomaticAllocationUsesHolesAndRecoversFromExhaustion)
{
    auto* sampler           = Sampler();
    const uint32_t capacity = GetBindlessHeapCapacity(RHIBindlessHeapType::eSampler);
    HeapVector<RHIBindlessHandle> handles;
    auto last = session->rhi.RegisterBindlessResource(sampler, capacity - 1);
    ASSERT_TRUE(last.IsValid());
    handles.push_back(last);
    for (uint32_t i = 0; i < capacity - 1; ++i)
    {
        auto handle = session->rhi.RegisterBindlessResource(sampler);
        ASSERT_TRUE(handle.IsValid());
        EXPECT_EQ(handle.slotIndex, i);
        handles.push_back(handle);
    }
    EXPECT_FALSE(session->rhi.RegisterBindlessResource(sampler).IsValid());
    EXPECT_EQ(sampler->GetRefCount(), capacity + 1);
    ASSERT_TRUE(session->rhi.UnregisterBindlessResource(handles[capacity / 2]));
    auto reused = session->rhi.RegisterBindlessResource(sampler);
    ASSERT_TRUE(reused.IsValid());
    EXPECT_EQ(reused.slotIndex, handles[capacity / 2].slotIndex);
    EXPECT_NE(reused.generation, handles[capacity / 2].generation);
    EXPECT_FALSE(session->rhi.RegisterBindlessResource(sampler).IsValid());
}

TEST_F(VulkanBindlessRetirementIntegrationTest, UnflushedRetirementReleasesViewAndOwner)
{
    auto* texture = Texture();
    auto* view    = texture->GetDefaultView();
    auto handle   = session->rhi.RegisterBindlessResource(view);
    ASSERT_TRUE(handle.IsValid());
    EXPECT_EQ(texture->GetRefCount(), 2u);
    EXPECT_EQ(view->GetRefCount(), 2u);
    ASSERT_TRUE(session->rhi.UnregisterBindlessResource(handle));
    EXPECT_EQ(texture->GetRefCount(), 1u);
    EXPECT_EQ(view->GetRefCount(), 1u);
    session->rhi.DestroyTexture(texture);
    textures.back() = nullptr;
    // A pending write must not touch the freed view, or overwrite a later registration.
    session->rhi.GetBindlessDescriptorPoolManager()->Flush();
    auto* replacement = Texture();
    auto next         = session->rhi.RegisterBindlessResource(replacement->GetDefaultView());
    EXPECT_EQ(next.slotIndex, handle.slotIndex);
    session->rhi.GetBindlessDescriptorPoolManager()->Flush();
    EXPECT_TRUE(session->rhi.UnregisterBindlessResource(next));
    EXPECT_EQ(replacement->GetRefCount(), 1u);
}

TEST_F(VulkanBindlessRetirementIntegrationTest, AllRecordedDrawAndDispatchFormsPinUntilRollback)
{
    RHISampler* sampler = Sampler();
    commandList   = RHICommandList::Create(context);
    for (uint32_t form = 0; form < 6; ++form)
    {
        SCOPED_TRACE(form);
        RHIBindlessHandle handle = session->rhi.RegisterBindlessResource(sampler, 0);
        ASSERT_TRUE(handle.IsValid());
        RHICommandListBase::CommandCheckpoint checkpoint = commandList->GetCommandCheckpoint();
        switch (form)
        {
            case 0: commandList->Draw(3, 1, 0, 0); break;
            case 1: commandList->DrawIndexed({}); break;
            case 2: commandList->DrawIndexedIndirect({}); break;
            case 3: commandList->Dispatch(1, 1, 1); break;
            case 4: commandList->DispatchIndirect(nullptr, 0); break;
            case 5:
            {
                RHIShaderResourceDescriptor descriptor{};
                descriptor.bindless = true;
                descriptor.type     = RHIShaderResourceType::eSampler;
                RHIBatchedShaderParameters parameters;
                parameters.AddResourceParam(descriptor, sampler, nullptr, 0);
                commandList->SetShaderParameters(parameters);
                break;
            }
        }
        ASSERT_TRUE(session->rhi.UnregisterBindlessResource(handle));
        // Explicit parameters own an additional reference; all forms also pin the heap epoch.
        EXPECT_EQ(sampler->GetRefCount(), form == 5 ? 3u : 2u);
        EXPECT_FALSE(session->rhi.RegisterBindlessResource(sampler, 0).IsValid());
        commandList->RollbackCommands(checkpoint);
        session->rhi.CollectRetiredBindlessResources();
        EXPECT_EQ(sampler->GetRefCount(), 1u);
    }
}

TEST_F(VulkanBindlessRetirementIntegrationTest, NewRecordingsDoNotKeepOlderRetirementsAlive)
{
    auto* sampler = Sampler();
    auto handle   = session->rhi.RegisterBindlessResource(sampler, 0);
    const uint64_t oldEpoch = context->RHICaptureBindlessEpoch();
    ASSERT_TRUE(session->rhi.UnregisterBindlessResource(handle));
    const uint64_t newEpoch = context->RHICaptureBindlessEpoch();
    context->RHIReleaseBindlessEpoch(oldEpoch);
    session->rhi.CollectRetiredBindlessResources();
    EXPECT_EQ(sampler->GetRefCount(), 1u);
    auto next = session->rhi.RegisterBindlessResource(sampler, 0);
    ASSERT_TRUE(next.IsValid());
    ASSERT_TRUE(session->rhi.UnregisterBindlessResource(next));
    EXPECT_EQ(sampler->GetRefCount(), 2u);
    context->RHIReleaseBindlessEpoch(newEpoch);
    session->rhi.CollectRetiredBindlessResources();
    EXPECT_EQ(sampler->GetRefCount(), 1u);
}

TEST_F(VulkanBindlessRetirementIntegrationTest, OnlyLiveEpochsFromThisManagerPermitReplay)
{
    auto* sampler        = Sampler();
    auto* manager        = session->rhi.GetBindlessDescriptorPoolManager();
    auto handle          = session->rhi.RegisterBindlessResource(sampler, 0);
    const uint64_t epoch = context->RHICaptureBindlessEpoch();
    EXPECT_TRUE(session->rhi.UnregisterBindlessResource(handle));
    const uint64_t newerEpoch = context->RHICaptureBindlessEpoch();
    EXPECT_TRUE(manager->RegisterBindlessResource(sampler, 0, nullptr, epoch));
    EXPECT_FALSE(manager->RegisterBindlessResource(sampler, 0, nullptr, newerEpoch));

    VulkanBindlessDescriptorPoolManager other;
    other.Init();
    const uint64_t foreignEpoch = other.CaptureEpoch();
    EXPECT_FALSE(manager->RegisterBindlessResource(sampler, 0, nullptr, foreignEpoch));
    other.ReleaseEpoch(foreignEpoch);
    other.Destroy();
    other.Init();
    const uint64_t recreatedEpoch = other.CaptureEpoch();
    RHIBindlessHandle otherHandle;
    EXPECT_TRUE(other.RegisterBindlessResource(sampler, 0, &otherHandle));
    EXPECT_TRUE(other.UnregisterBindlessResource(otherHandle));
    EXPECT_FALSE(other.RegisterBindlessResource(sampler, 0, nullptr, foreignEpoch));
    EXPECT_TRUE(other.RegisterBindlessResource(sampler, 0, nullptr, recreatedEpoch));
    other.ReleaseEpoch(recreatedEpoch);
    other.Destroy();

    context->RHIReleaseBindlessEpoch(epoch);
    context->RHIReleaseBindlessEpoch(newerEpoch);
    session->rhi.CollectRetiredBindlessResources();
    EXPECT_EQ(sampler->GetRefCount(), 1u);
}

TEST_F(VulkanBindlessRetirementIntegrationTest, RecordedParametersAndReplayRetainOldGPUValues)
{
    auto* pipeline = Compute("binding_bindless.comp.spv");
    auto* texture  = Texture();
    auto* sampler  = Sampler();
    auto* output   = Buffer();
    Initialize(texture);
    SubmitAndWait(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    auto imageHandle   = session->rhi.RegisterBindlessResource(texture->GetDefaultView(), 0);
    auto samplerHandle = session->rhi.RegisterBindlessResource(sampler, 0);
    ASSERT_TRUE(imageHandle.IsValid());
    ASSERT_TRUE(samplerHandle.IsValid());
    commandList = RHICommandList::Create(context);
    commandList->BindPipeline(RHIPipelineType::eCompute, pipeline);
    RHIBatchedShaderParameters parameters;
    parameters.AddResourceParam(*pipeline->GetShader()->GetSRDByLocation(0, 0),
                                texture->GetDefaultView(), nullptr, 0);
    parameters.AddResourceParam(*pipeline->GetShader()->GetSRDByLocation(0, 2), sampler, nullptr,
                                0);
    parameters.AddResourceParam(*pipeline->GetShader()->GetSRDByLocation(2, 0), output, nullptr, 0);
    commandList->SetShaderParameters(parameters);
    const uint32_t index = 0;
    commandList->SetPushConstants(pipeline, reinterpret_cast<const uint8_t*>(&index), sizeof(index),
                                  0);
    commandList->Dispatch(1, 1, 1);
    ASSERT_TRUE(session->rhi.UnregisterBindlessResource(imageHandle));
    ASSERT_TRUE(session->rhi.UnregisterBindlessResource(samplerHandle));
    session->rhi.DestroyTexture(texture);
    textures.back() = nullptr;
    session->rhi.DestroySampler(sampler);
    samplers.back() = nullptr;
    for (uint32_t replay = 0; replay < 2; ++replay)
    {
        commandList->Execute();
        SubmitAndWait();
        CheckPixel(output, 0xFF0000FFu);
        EXPECT_FALSE(session->rhi.RegisterBindlessResource(sampler, 0).IsValid());
    }
    auto* nextTexture = Texture();
    auto* nextSampler = Sampler();
    commandList->Reset();
    session->rhi.CollectRetiredBindlessResources();
    ASSERT_TRUE(session->rhi.RegisterBindlessResource(nextTexture->GetDefaultView(), 0).IsValid());
    ASSERT_TRUE(session->rhi.RegisterBindlessResource(nextSampler, 0).IsValid());
    Initialize(nextTexture, true);
    SetOutput(pipeline, output, 2, 0);
    PushIndex(pipeline, 0);
    context->RHIDispatch(1, 1, 1);
    SubmitAndWait();
    CheckPixel(output, 0xFF00FF00u);
}

TEST_F(VulkanBindlessRetirementIntegrationTest,
       RepeatedTextureAndSamplerStreamingExceedsHeapCapacity)
{
    const uint32_t cycles = GetBindlessHeapCapacity(RHIBindlessHeapType::eTexture2D) + 32;
    for (uint32_t i = 0; i < cycles; ++i)
    {
        SCOPED_TRACE(i);
        auto* texture = Texture();
        auto* sampler = Sampler();
        auto image    = session->rhi.RegisterBindlessResource(texture->GetDefaultView());
        auto sampling = session->rhi.RegisterBindlessResource(sampler);
        ASSERT_TRUE(image.IsValid());
        ASSERT_TRUE(sampling.IsValid());
        EXPECT_EQ(image.slotIndex, 0u);
        EXPECT_EQ(sampling.slotIndex, 0u);
        if (i % 2 == 0)
        {
            session->rhi.GetBindlessDescriptorPoolManager()->Flush();
        }
        EXPECT_TRUE(session->rhi.UnregisterBindlessResource(image));
        EXPECT_TRUE(session->rhi.UnregisterBindlessResource(sampling));
        EXPECT_EQ(texture->GetRefCount(), 1u);
        EXPECT_EQ(texture->GetDefaultView()->GetRefCount(), 1u);
        EXPECT_EQ(sampler->GetRefCount(), 1u);
        session->rhi.DestroyTexture(texture);
        textures.pop_back();
        session->rhi.DestroySampler(sampler);
        samplers.pop_back();
    }
    session->rhi.GetBindlessDescriptorPoolManager()->Flush();
}

TEST_F(VulkanBindlessRetirementIntegrationTest, ContextDestructionDiscardsUnsubmittedHeapUse)
{
    auto* sampler = Sampler();
    auto handle   = session->rhi.RegisterBindlessResource(sampler, 0);
    ASSERT_TRUE(handle.IsValid());
    context->RecordCurrentBindlessEpoch();
    context->GetCommandBuffer();
    ASSERT_TRUE(session->rhi.UnregisterBindlessResource(handle));
    EXPECT_EQ(sampler->GetRefCount(), 2u);
    ZEN_DELETE(context);
    context = nullptr;
    session->rhi.CollectRetiredBindlessResources();
    EXPECT_EQ(sampler->GetRefCount(), 1u);
    EXPECT_TRUE(session->rhi.RegisterBindlessResource(sampler, 0).IsValid());
}

TEST_F(VulkanBindlessRetirementIntegrationTest, CubeRegistrationsReleaseTheirImageOwners)
{
    RHITextureCreateInfo info{};
    info.type  = RHITextureType::eCube;
    info.width = info.height = 4;
    info.arrayLayers         = 6;
    info.format              = DataFormat::eR8G8B8A8UNORM;
    info.usageFlags.SetFlag(RHITextureUsageFlagBits::eSampled);
    auto* texture = session->rhi.CreateTexture(info);
    textures.push_back(texture);
    for (uint32_t i = 0; i < GetBindlessHeapCapacity(RHIBindlessHeapType::eTextureCube) + 1; ++i)
    {
        auto handle = session->rhi.RegisterBindlessResource(texture->GetDefaultView());
        ASSERT_TRUE(handle.IsValid());
        EXPECT_EQ(handle.heapType, RHIBindlessHeapType::eTextureCube);
        EXPECT_EQ(handle.slotIndex, 0u);
        session->rhi.GetBindlessDescriptorPoolManager()->Flush();
        EXPECT_TRUE(session->rhi.UnregisterBindlessResource(handle));
        EXPECT_EQ(texture->GetRefCount(), 1u);
        EXPECT_EQ(texture->GetDefaultView()->GetRefCount(), 1u);
    }
}

// Delay the real GPU without replacing any Vulkan completion query. Cleanup
// always opens the gate before waiting, including when an assertion fails.
class HostGate
{
public:
    explicit HostGate(VulkanDevice* device) : device(device)
    {
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        info.pNext = &type;
        EXPECT_EQ(vkCreateSemaphore(device->GetVkHandle(), &info, nullptr, &semaphore), VK_SUCCESS);
    }
    ~HostGate()
    {
        Open();
        device->WaitForIdle();
        vkDestroySemaphore(device->GetVkHandle(), semaphore, nullptr);
    }
    void Record(FVulkanCommandListContext* context)
    {
        // A semaphore wait also gates all later commands in queue submission order.
        // This test-only native prelude works with either engine completion backend.
        const uint64_t value = 1;
        VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        timeline.waitSemaphoreValueCount = 1;
        timeline.pWaitSemaphoreValues    = &value;
        const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.pNext              = &timeline;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores    = &semaphore;
        submit.pWaitDstStageMask  = &stage;
        EXPECT_EQ(vkQueueSubmit(context->GetQueue()->GetVkHandle(), 1, &submit, VK_NULL_HANDLE),
                  VK_SUCCESS);
    }
    void Open()
    {
        if (!opened)
        {
            VkSemaphoreSignalInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
            info.semaphore = semaphore;
            info.value     = 1;
            EXPECT_EQ(vkSignalSemaphore(device->GetVkHandle(), &info), VK_SUCCESS);
            opened = true;
        }
    }

private:
    VulkanDevice* device;
    VkSemaphore semaphore{};
    bool opened{false};
};

class VulkanBindlessQueueRetirementTest :
    public VulkanBindlessRetirementIntegrationTest,
    public testing::WithParamInterface<bool>
{
    void SetUp() override
    {
        VulkanBindlessRetirementIntegrationTest::SetUp();
        ASSERT_TRUE(session->rhi.GetDevice()->SupportsTimelineSemaphore());
        // Exercise the existing fence backend on the same real device as well.
        session->rhi.GetDevice()->GetExtensionFlags().hasTimelineSemaphore = GetParam();
    }
};

TEST_P(VulkanBindlessQueueRetirementTest, PendingGPUReadKeepsViewAndSamplerAlive)
{
    auto* pipeline = Compute("binding_bindless.comp.spv");
    auto* texture  = Texture();
    auto* sampler  = Sampler();
    auto* output   = Buffer();
    Initialize(texture);
    SubmitAndWait(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    auto image    = session->rhi.RegisterBindlessResource(texture->GetDefaultView(), 0);
    auto sampling = session->rhi.RegisterBindlessResource(sampler, 0);
    ASSERT_TRUE(image.IsValid());
    ASSERT_TRUE(sampling.IsValid());
    HostGate gate(session->rhi.GetDevice());
    gate.Record(context);
    SetOutput(pipeline, output, 2, 0);
    PushIndex(pipeline, 0);
    context->RHIDispatch(1, 1, 1);
    ASSERT_TRUE(session->rhi.UnregisterBindlessResource(image));
    ASSERT_TRUE(session->rhi.UnregisterBindlessResource(sampling));
    session->rhi.DestroyTexture(texture);
    textures.back() = nullptr;
    session->rhi.DestroySampler(sampler);
    samplers.back()   = nullptr;
    auto* replacement = Texture();
    auto* nextSampler = Sampler();
    EXPECT_FALSE(session->rhi.RegisterBindlessResource(replacement, 0).IsValid());
    Enqueue(context);
    uint64_t serial = 0;
    auto* queue     = context->GetQueue();
    ASSERT_EQ(queue->SubmitPendingWorkloads(serial), RHISubmissionResult::eSuccess);
    EXPECT_FALSE(queue->WaitForSubmission(serial, 0));
    session->rhi.CollectRetiredBindlessResources();
    EXPECT_FALSE(session->rhi.RegisterBindlessResource(replacement, 0).IsValid());
    EXPECT_FALSE(session->rhi.RegisterBindlessResource(nextSampler, 0).IsValid());
    gate.Open();
    ASSERT_TRUE(queue->WaitForSubmission(serial, UINT64_MAX));
    // Submit a host-read barrier after the now-completed compute write.
    SubmitAndWait();
    CheckPixel(output, 0xFF0000FFu);
    EXPECT_TRUE(session->rhi.RegisterBindlessResource(replacement, 0).IsValid());
    EXPECT_TRUE(session->rhi.RegisterBindlessResource(nextSampler, 0).IsValid());
}

TEST_P(VulkanBindlessQueueRetirementTest, EveryQueueMustCompleteItsRecordedEpoch)
{
    auto* sampler = Sampler();
    auto handle   = session->rhi.RegisterBindlessResource(sampler, 0);
    ASSERT_TRUE(handle.IsValid());
    auto* compute = static_cast<FVulkanCommandListContext*>(
        session->rhi.GetCommandContext(RHICommandContextType::eAsyncCompute));
    const uint64_t epoch = context->RHICaptureBindlessEpoch();
    context->RecordLifetime(epoch);
    compute->RecordLifetime(epoch);
    context->GetCommandBuffer();
    compute->GetCommandBuffer();
    context->RHIReleaseBindlessEpoch(epoch);
    ASSERT_TRUE(session->rhi.UnregisterBindlessResource(handle));
    Enqueue(context);
    uint64_t serial = 0;
    ASSERT_EQ(context->GetQueue()->SubmitPendingWorkloads(serial), RHISubmissionResult::eSuccess);
    ASSERT_TRUE(context->GetQueue()->WaitForSubmission(serial, UINT64_MAX));
    EXPECT_FALSE(session->rhi.RegisterBindlessResource(sampler, 0).IsValid());
    HostGate gate(session->rhi.GetDevice());
    gate.Record(compute);
    Enqueue(compute);
    ASSERT_EQ(compute->GetQueue()->SubmitPendingWorkloads(serial), RHISubmissionResult::eSuccess);
    EXPECT_FALSE(compute->GetQueue()->WaitForSubmission(serial, 0));
    session->rhi.CollectRetiredBindlessResources();
    EXPECT_FALSE(session->rhi.RegisterBindlessResource(sampler, 0).IsValid());
    gate.Open();
    ASSERT_TRUE(compute->GetQueue()->WaitForSubmission(serial, UINT64_MAX));
    EXPECT_TRUE(session->rhi.RegisterBindlessResource(sampler, 0).IsValid());
    ZEN_DELETE(compute);
}

static VKAPI_ATTR VkResult VKAPI_CALL RejectSubmit(VkQueue, uint32_t, const VkSubmitInfo*, VkFence)
{
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

TEST_P(VulkanBindlessQueueRetirementTest, ReplayResetWaitsForLatestSubmission)
{
    auto* pipeline = Compute("binding_bindless.comp.spv");
    auto* texture  = Texture();
    auto* sampler  = Sampler();
    auto* output   = Buffer();
    Initialize(texture);
    SubmitAndWait(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    auto image    = session->rhi.RegisterBindlessResource(texture->GetDefaultView(), 0);
    auto sampling = session->rhi.RegisterBindlessResource(sampler, 0);
    ASSERT_TRUE(image.IsValid());
    ASSERT_TRUE(sampling.IsValid());
    SetOutput(pipeline, output, 2, 0);
    commandList          = RHICommandList::Create(context);
    const uint32_t index = 0;
    commandList->SetPushConstants(pipeline, reinterpret_cast<const uint8_t*>(&index), sizeof(index),
                                  0);
    commandList->Dispatch(1, 1, 1);
    const uint64_t epoch = context->RHICaptureBindlessEpoch();
    ASSERT_TRUE(session->rhi.UnregisterBindlessResource(image));
    ASSERT_TRUE(session->rhi.UnregisterBindlessResource(sampling));
    commandList->Execute();
    SubmitAndWait();
    EXPECT_FALSE(session->rhi.RegisterBindlessResource(sampler, 0).IsValid());

    HostGate gate(session->rhi.GetDevice());
    gate.Record(context);
    commandList->Execute();
    Enqueue(context);
    uint64_t serial = 0;
    auto* queue     = context->GetQueue();
    ASSERT_EQ(queue->SubmitPendingWorkloads(serial), RHISubmissionResult::eSuccess);
    commandList->Reset();
    context->RHIReleaseBindlessEpoch(epoch);
    EXPECT_FALSE(queue->WaitForSubmission(serial, 0));
    session->rhi.CollectRetiredBindlessResources();
    EXPECT_FALSE(session->rhi.RegisterBindlessResource(sampler, 0).IsValid());
    // A saved integer alone cannot reactivate a recording after its count is released.
    EXPECT_FALSE(session->rhi.GetBindlessDescriptorPoolManager()->RegisterBindlessResource(
        sampler, 0, nullptr, epoch));
    gate.Open();
    ASSERT_TRUE(queue->WaitForSubmission(serial, UINT64_MAX));
    SubmitAndWait();
    CheckPixel(output, 0xFF0000FFu);
    EXPECT_TRUE(session->rhi.RegisterBindlessResource(sampler, 0).IsValid());
}

TEST_P(VulkanBindlessQueueRetirementTest, RejectedMergedEpochCountsTransferOnRetry)
{
    auto* sampler = Sampler();
    auto handle   = session->rhi.RegisterBindlessResource(sampler, 0);
    auto* other   = static_cast<FVulkanCommandListContext*>(
        session->rhi.GetCommandContext(RHICommandContextType::eGraphics));
    const uint64_t epoch = context->RHICaptureBindlessEpoch();
    context->RecordLifetime(epoch);
    context->RecordLifetime(epoch); // Deduplicate within one recording.
    other->RecordLifetime(epoch);   // Preserve the other recording's count when merging.
    context->GetCommandBuffer();
    other->GetCommandBuffer();
    context->RHIReleaseBindlessEpoch(epoch);
    EXPECT_TRUE(session->rhi.UnregisterBindlessResource(handle));
    Enqueue(context);
    Enqueue(other);
    uint64_t serial = 0;
    auto* queue     = context->GetQueue();
    {
        test::ScopedVulkanCall<PFN_vkQueueSubmit> reject(vkQueueSubmit, RejectSubmit);
        EXPECT_EQ(queue->SubmitPendingWorkloads(serial), RHISubmissionResult::eRejected);
    }
    EXPECT_EQ(serial, 0u);
    EXPECT_FALSE(session->rhi.RegisterBindlessResource(sampler, 0).IsValid());
    HostGate gate(session->rhi.GetDevice());
    gate.Record(context);
    EXPECT_EQ(queue->SubmitPendingWorkloads(serial), RHISubmissionResult::eSuccess);
    EXPECT_FALSE(queue->WaitForSubmission(serial, 0));
    EXPECT_FALSE(session->rhi.RegisterBindlessResource(sampler, 0).IsValid());
    gate.Open();
    EXPECT_TRUE(queue->WaitForSubmission(serial, UINT64_MAX));
    EXPECT_TRUE(session->rhi.RegisterBindlessResource(sampler, 0).IsValid());
    ZEN_DELETE(other);
}

TEST_P(VulkanBindlessQueueRetirementTest, OneSubmissionProtectsUniformsBindlessAndDescriptorPools)
{
    auto& tracker   = session->rhi.GetLifetimeTracker();
    auto* allocator = session->rhi.GetUniformBufferAllocator();
    auto* pools     = session->rhi.GetDescriptorPoolManager2();
    allocator->BeginFrame(0);
    const auto allocation = allocator->Alloc(64);
    ASSERT_TRUE(allocation.IsValid());
    auto* pool    = pools->AcquireDescriptorPoolSetContainer();
    auto* sampler = Sampler();
    auto handle   = session->rhi.RegisterBindlessResource(sampler, 0);
    ASSERT_TRUE(handle.IsValid());
    const uint64_t epoch         = context->RHICaptureBindlessEpoch();
    const uint64_t blockLifetime = allocator->GetBlockLifetime(allocation.blockId);
    EXPECT_NE(blockLifetime, epoch);
    EXPECT_NE(pool->GetLifetimeId(), epoch);
    EXPECT_NE(pool->GetLifetimeId(), blockLifetime);
    context->RecordUniformBufferBlock(allocation.blockId);
    context->RecordDescriptorPool(pool);
    context->RecordLifetime(epoch);
    context->RHIReleaseBindlessEpoch(epoch);
    pools->ReleaseContainer(pool);
    ASSERT_TRUE(session->rhi.UnregisterBindlessResource(handle));
    HostGate gate(session->rhi.GetDevice());
    gate.Record(context);
    context->GetCommandBuffer();
    Enqueue(context);
    auto* queue     = context->GetQueue();
    uint64_t serial = 0;
    ASSERT_EQ(queue->SubmitPendingWorkloads(serial), RHISubmissionResult::eSuccess);
    EXPECT_FALSE(queue->WaitForSubmission(serial, 0));
    EXPECT_FALSE(tracker.HasRecordings(blockLifetime));
    EXPECT_FALSE(tracker.HasRecordings(epoch));
    EXPECT_FALSE(tracker.HasRecordings(pool->GetLifetimeId()));
    EXPECT_FALSE(tracker.IsComplete(blockLifetime));
    EXPECT_FALSE(pool->CanReuse());
    EXPECT_FALSE(session->rhi.RegisterBindlessResource(sampler, 0).IsValid());
    allocator->BeginFrame(0);
    EXPECT_NE(allocator->Alloc(64).blockId, allocation.blockId);
    gate.Open();
    ASSERT_TRUE(queue->WaitForSubmission(serial, UINT64_MAX));
    EXPECT_TRUE(pool->CanReuse());
    EXPECT_TRUE(session->rhi.RegisterBindlessResource(sampler, 0).IsValid());
    allocator->BeginFrame(0);
    EXPECT_EQ(allocator->Alloc(64).blockId, allocation.blockId);
}

TEST_P(VulkanBindlessQueueRetirementTest, RejectedMergedWorkRetainsUntilExplicitDiscard)
{
    auto* first       = Sampler();
    auto* second      = Sampler();
    auto firstHandle  = session->rhi.RegisterBindlessResource(first, 0);
    auto secondHandle = session->rhi.RegisterBindlessResource(second, 1);
    auto* other       = static_cast<FVulkanCommandListContext*>(
        session->rhi.GetCommandContext(RHICommandContextType::eGraphics));
    const uint64_t epoch = context->RHICaptureBindlessEpoch();
    context->RecordLifetime(epoch);
    other->RecordLifetime(epoch);
    context->GetCommandBuffer();
    other->GetCommandBuffer();
    context->RHIReleaseBindlessEpoch(epoch);
    EXPECT_TRUE(session->rhi.UnregisterBindlessResource(firstHandle));
    EXPECT_TRUE(session->rhi.UnregisterBindlessResource(secondHandle));
    Enqueue(context);
    Enqueue(other);
    uint64_t serial = 0;
    auto* queue     = context->GetQueue();
    {
        test::ScopedVulkanCall<PFN_vkQueueSubmit> reject(vkQueueSubmit, RejectSubmit);
        EXPECT_EQ(queue->SubmitPendingWorkloads(serial), RHISubmissionResult::eRejected);
    }
    EXPECT_EQ(serial, 0u);
    EXPECT_FALSE(session->rhi.RegisterBindlessResource(first, 0).IsValid());
    EXPECT_FALSE(session->rhi.RegisterBindlessResource(second, 1).IsValid());
    queue->DiscardPendingWorkloads();
    EXPECT_TRUE(session->rhi.RegisterBindlessResource(first, 0).IsValid());
    EXPECT_TRUE(session->rhi.RegisterBindlessResource(second, 1).IsValid());
    ZEN_DELETE(other);
}

TEST_P(VulkanBindlessQueueRetirementTest, UncertainSubmissionDoesNotPermitEarlyReclamation)
{
    auto* sampler = Sampler();
    auto handle   = session->rhi.RegisterBindlessResource(sampler, 0);
    ASSERT_TRUE(handle.IsValid());
    context->RecordCurrentBindlessEpoch();
    context->GetCommandBuffer();
    ASSERT_TRUE(session->rhi.UnregisterBindlessResource(handle));
    Enqueue(context);
    uint64_t serial = 0;
    auto* queue     = context->GetQueue();
    {
        test::ScopedVulkanCall<PFN_vkQueueSubmit> fail(
            vkQueueSubmit, +[](VkQueue, uint32_t, const VkSubmitInfo*, VkFence) -> VkResult {
                return VK_ERROR_DEVICE_LOST;
            });
        EXPECT_EQ(queue->SubmitPendingWorkloads(serial), RHISubmissionResult::eFatal);
    }
    queue->DiscardPendingWorkloads(true);
    session->rhi.CollectRetiredBindlessResources();
    EXPECT_EQ(sampler->GetRefCount(), 2u);
    EXPECT_FALSE(session->rhi.RegisterBindlessResource(sampler, 0).IsValid());
    // Uncertain work retains its pending epoch count until queue/device teardown.
}

INSTANTIATE_TEST_SUITE_P(TimelineAndFence, VulkanBindlessQueueRetirementTest, testing::Bool());
} // namespace
