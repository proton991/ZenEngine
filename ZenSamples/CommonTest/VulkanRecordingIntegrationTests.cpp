#include "VulkanIntegrationFixture.h"
#include "ScopedVulkanCall.h"
#include "Graphics/VulkanRHI/VulkanBuffer.h"
#include "Graphics/VulkanRHI/VulkanCommandList.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanPipeline.h"
#include "Graphics/VulkanRHI/VulkanTexture.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>

namespace
{
using namespace zen;
using Clock = std::chrono::steady_clock;

// Count calls at the public Vulkan boundary, forwarding all work to the driver.
template <typename Function> struct CountedVulkanCall;
template <typename Result, typename... Args> struct CountedVulkanCall<Result(VKAPI_PTR*)(Args...)>
{
    using Function = Result(VKAPI_PTR*)(Args...);
    static inline Function original;
    static inline uint32_t calls;
    test::ScopedVulkanCall<Function> hook;

    explicit CountedVulkanCall(Function& entry) : hook(entry, Save(entry)) {}
    static Function Save(Function entry)
    {
        original = entry;
        calls    = 0;
        return Invoke;
    }
    static std::unique_ptr<CountedVulkanCall> Create(Function& entry)
    {
        return std::make_unique<CountedVulkanCall>(entry);
    }
    static VKAPI_ATTR Result VKAPI_CALL Invoke(Args... args)
    {
        ++calls;
        return original(args...);
    }
};

class VulkanRecordingIntegrationTest : public testing::Test
{
protected:
    std::unique_ptr<test::VulkanSession> session;
    FVulkanCommandListContext* context{};
    VkDebugUtilsMessengerEXT messenger{};
    HeapVector<RHITexture*> textures;
    HeapVector<RHIBuffer*> buffers;
    HeapVector<RHIShader*> shaders;
    HeapVector<RHIPipeline*> pipelines;
    std::unique_ptr<CountedVulkanCall<PFN_vkCmdBindPipeline>> bindPipeline;
    std::unique_ptr<CountedVulkanCall<PFN_vkCmdBindDescriptorSets>> bindDescriptors;
    std::unique_ptr<CountedVulkanCall<PFN_vkUpdateDescriptorSets>> updateDescriptors;
    std::unique_ptr<CountedVulkanCall<PFN_vkCmdSetViewport>> viewport;
    std::unique_ptr<CountedVulkanCall<PFN_vkCmdSetScissor>> scissor;
    std::unique_ptr<CountedVulkanCall<PFN_vkCmdSetDepthBias>> depthBias;
    std::unique_ptr<CountedVulkanCall<PFN_vkCmdSetLineWidth>> lineWidth;
    std::unique_ptr<CountedVulkanCall<PFN_vkCmdSetBlendConstants>> blendConstants;
    std::unique_ptr<CountedVulkanCall<PFN_vkCmdBindVertexBuffers>> vertices;
    std::unique_ptr<CountedVulkanCall<PFN_vkCmdDraw>> draw;

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
        bindPipeline = CountedVulkanCall<PFN_vkCmdBindPipeline>::Create(vkCmdBindPipeline);
        bindDescriptors =
            CountedVulkanCall<PFN_vkCmdBindDescriptorSets>::Create(vkCmdBindDescriptorSets);
        updateDescriptors =
            CountedVulkanCall<PFN_vkUpdateDescriptorSets>::Create(vkUpdateDescriptorSets);
        viewport  = CountedVulkanCall<PFN_vkCmdSetViewport>::Create(vkCmdSetViewport);
        scissor   = CountedVulkanCall<PFN_vkCmdSetScissor>::Create(vkCmdSetScissor);
        depthBias = CountedVulkanCall<PFN_vkCmdSetDepthBias>::Create(vkCmdSetDepthBias);
        lineWidth = CountedVulkanCall<PFN_vkCmdSetLineWidth>::Create(vkCmdSetLineWidth);
        blendConstants =
            CountedVulkanCall<PFN_vkCmdSetBlendConstants>::Create(vkCmdSetBlendConstants);
        vertices = CountedVulkanCall<PFN_vkCmdBindVertexBuffers>::Create(vkCmdBindVertexBuffers);
        draw     = CountedVulkanCall<PFN_vkCmdDraw>::Create(vkCmdDraw);
    }

    void TearDown() override
    {
        session->rhi.WaitDeviceIdle();
        ZEN_DELETE(context);
        for (auto* value : pipelines)
        {
            session->rhi.DestroyPipeline(value);
        }
        for (auto* value : shaders)
        {
            session->rhi.DestroyShader(value);
        }
        for (auto* value : textures)
        {
            session->rhi.DestroyTexture(value);
        }
        for (auto* value : buffers)
        {
            session->rhi.DestroyBuffer(value);
        }
        draw.reset();
        vertices.reset();
        blendConstants.reset();
        lineWidth.reset();
        depthBias.reset();
        scissor.reset();
        viewport.reset();
        updateDescriptors.reset();
        bindDescriptors.reset();
        bindPipeline.reset();
        vkDestroyDebugUtilsMessengerEXT(session->rhi.GetInstance(), messenger, nullptr);
        session.reset();
    }

    VulkanTexture* Texture()
    {
        RHITextureCreateInfo info{};
        info.type   = RHITextureType::e2D;
        info.format = DataFormat::eR8G8B8A8UNORM;
        info.width = info.height = 8;
        info.usageFlags.SetFlags(RHITextureUsageFlagBits::eColorAttachment,
                                 RHITextureUsageFlagBits::eTransferSrc);
        auto* texture = static_cast<VulkanTexture*>(session->rhi.CreateTexture(info));
        textures.push_back(texture);
        return texture;
    }

    VulkanBuffer* Buffer(uint32_t size = 4096)
    {
        RHIBufferCreateInfo info{};
        info.size         = size;
        info.allocateType = RHIBufferAllocateType::eCPURead;
        info.usageFlags.SetFlags(
            RHIBufferUsageFlagBits::eUniformBuffer, RHIBufferUsageFlagBits::eVertexBuffer,
            RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferDstBuffer);
        auto* buffer = static_cast<VulkanBuffer*>(session->rhi.CreateBuffer(info));
        buffers.push_back(buffer);
        return buffer;
    }

    VulkanShader* Shader(const char* fragment   = "recording_uniform.frag.spv",
                         bool compute           = false,
                         const char* vertexFile = "pipeline.vert.spv")
    {
        RHIShaderCreateInfo info{};
        auto stage = [&](RHIShaderStage value, const char* file) {
            info.stageFlags.SetFlag(RHIShaderStageToFlagBits(value));
            info.spirvFileName[ToUnderlying(value)] =
                std::filesystem::relative(std::filesystem::path(RDG_REFLECTION_TEST_PATH) / file,
                                          SPV_SHADER_PATH)
                    .generic_string();
        };
        if (!compute)
        {
            stage(RHIShaderStage::eVertex, vertexFile);
        }
        stage(compute ? RHIShaderStage::eCompute : RHIShaderStage::eFragment, fragment);
        auto* shader = static_cast<VulkanShader*>(session->rhi.CreateShader(info));
        shaders.push_back(shader);
        return shader;
    }

    RHIRenderingLayout Layout(VulkanTexture* texture)
    {
        RHIRenderingLayout layout{};
        layout.SetRenderArea(0, 0, 8, 8);
        layout.AddColorRenderTarget(texture->GetFormat(), texture, RHIRenderTargetLoadOp::eClear,
                                    RHIRenderTargetStoreOp::eStore,
                                    RHIRenderTargetClearValue(Color(0, 0, 0, 0)));
        return layout;
    }

    RHIGfxPipelineCreateInfo PipelineInfo(RHIShader* shader,
                                          const RHIRenderingLayout& layout,
                                          bool dynamic = true)
    {
        RHIGfxPipelineCreateInfo info{};
        info.pShader          = shader;
        info.pRenderingLayout = &layout;
        info.states.colorBlendState.AddAttachment();
        if (dynamic)
        {
            info.states.dynamicStates.Enable(RHIDynamicState::eViewPort, RHIDynamicState::eScissor,
                                             RHIDynamicState::eDepthBias,
                                             RHIDynamicState::eLineWidth);
            info.states.rasterizationState.enableDepthBias = true;
        }
        return info;
    }

    RHIPipeline* Graphics(const RHIGfxPipelineCreateInfo& info)
    {
        auto* pipeline = session->rhi.CreatePipeline(info);
        pipelines.push_back(pipeline);
        return pipeline;
    }

    RHIBatchedShaderParameters Parameters(RHIShader* shader, RHIBuffer* buffer, uint32_t offset = 0)
    {
        RHIBatchedShaderParameters parameters;
        parameters.AddResourceParam(*shader->GetSRDByLocation(test::kLocalResourceSet, 0), buffer,
                                    nullptr, 0, offset);
        return parameters;
    }

    void Uniform(VulkanBuffer* buffer, uint32_t offset, const Color& color)
    {
        memcpy(buffer->Map() + offset, &color, sizeof(color));
        buffer->Unmap();
    }

    VkCommandBuffer Commands()
    {
        return context->GetCommandBuffer()->GetVkHandle();
    }

    void Transition(VulkanTexture* texture, VkImageLayout before, VkImageLayout after)
    {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask =
            before == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask       = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.oldLayout           = before;
        barrier.newLayout           = after;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image                                             = texture->GetVkImage();
        barrier.subresourceRange = texture->GetVkSubresourceRange();
        vkCmdPipelineBarrier(Commands(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);
    }

    void SubmitAndWait()
    {
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(Commands(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        HeapVector<VulkanWorkload*> workloads;
        context->CollectWorkloads(workloads);
        auto* queue = session->rhi.GetDevice()->GetGfxQueue();
        for (auto* workload : workloads)
        {
            queue->EnqueueWorkload(workload);
        }
        uint64_t serial = 0;
        ASSERT_EQ(queue->SubmitPendingWorkloads(serial), RHISubmissionResult::eSuccess);
        ASSERT_TRUE(queue->WaitForCompletion(serial, UINT64_MAX));
    }

    VulkanBuffer* Copy(VulkanTexture* texture)
    {
        Transition(texture, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        auto* buffer = Buffer();
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent      = {8, 8, 1};
        vkCmdCopyImageToBuffer(Commands(), texture->GetVkImage(),
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer->GetVkBuffer(), 1,
                               &copy);
        return buffer;
    }

    void DynamicState()
    {
        context->RHISetViewport(0, 0, 8, 8);
        context->RHISetScissor(0, 0, 8, 8);
        context->RHISetLineWidth(1);
        context->RHISetDepthBias(0, 0, 0);
    }

    void CheckPixel(VulkanBuffer* buffer, uint32_t x, uint32_t y, const Color& color)
    {
        const uint8_t* pixel = buffer->Map() + (y * 8 + x) * 4;
        EXPECT_EQ(pixel[0], uint8_t(color.r * 255));
        EXPECT_EQ(pixel[1], uint8_t(color.g * 255));
        EXPECT_EQ(pixel[2], uint8_t(color.b * 255));
        EXPECT_EQ(pixel[3], uint8_t(color.a * 255));
        buffer->Unmap();
    }

    void ResetCounts()
    {
        bindPipeline->calls = bindDescriptors->calls = updateDescriptors->calls = 0;
        viewport->calls = scissor->calls = depthBias->calls = lineWidth->calls = 0;
        blendConstants->calls = vertices->calls = draw->calls = 0;
    }
};

TEST_F(VulkanRecordingIntegrationTest, PackedUniformGrowthPreservesEarlierValuesAndReusesBlocks)
{
    auto* shader = Shader("recording_uniform.comp.spv", true);
    RHIComputePipelineCreateInfo info{};
    info.pShader   = shader;
    auto* pipeline = session->rhi.CreatePipeline(info);
    pipelines.push_back(pipeline);
    auto* first     = Buffer();
    auto* second    = Buffer();
    auto* allocator = session->rhi.GetUniformBufferAllocator();
    HeapVector<RHIBuffer*> previousBlocks;
    context->RHIBindPipeline(pipeline);

    auto dispatch = [&](VulkanBuffer* output, uint32_t value) {
        const uint32_t values[4]{value, 0, 0, 0};
        RHIBatchedShaderParameters parameters;
        parameters.AddValueParam(*shader->GetSRDByLocation(test::kLocalResourceSet, 0), values,
                                 sizeof(values));
        parameters.AddResourceParam(*shader->GetSRDByLocation(test::kLocalResourceSet, 1), output,
                                    nullptr, 0);
        context->RHISetShaderParameters(parameters);
        context->RHIDispatch(1, 1, 1);
    };

    for (uint32_t round = 0; round < 2; ++round)
    {
        SCOPED_TRACE(round);
        // The prior round has completed before this slot's storage is reused.
        allocator->BeginFrame(0);
        dispatch(first, 7 + round);
        HeapVector<RHIBuffer*> blocks;
        auto allocation = allocator->Alloc(16);
        ASSERT_TRUE(allocation.IsValid());
        blocks.push_back(allocation.pBuffer);
        // Fill blocks 1..7 after the first dispatch has used part of block 0.
        // The next packed update must grow past the former eight-block limit.
        for (uint32_t block = 0; block < 7; ++block)
        {
            allocation = allocator->Alloc(4 * 1024 * 1024);
            ASSERT_TRUE(allocation.IsValid());
            blocks.push_back(allocation.pBuffer);
        }
        dispatch(second, 29 + round);
        allocation = allocator->Alloc(16);
        ASSERT_TRUE(allocation.IsValid());
        blocks.push_back(allocation.pBuffer);
        SubmitAndWait();
        const uint32_t firstValue = *reinterpret_cast<const uint32_t*>(first->Map());
        first->Unmap();
        const uint32_t secondValue = *reinterpret_cast<const uint32_t*>(second->Map());
        second->Unmap();
        EXPECT_EQ(firstValue, 7u + round);
        EXPECT_EQ(secondValue, 29u + round);
        // Compare actual buffer identities to verify reuse between completed frames.
        if (round != 0)
        {
            ASSERT_EQ(blocks.size(), previousBlocks.size());
            for (uint32_t i = 0; i < blocks.size(); ++i)
            {
                EXPECT_EQ(blocks[i], previousBlocks[i]);
            }
        }
        previousBlocks = std::move(blocks);
    }
}

class VulkanUniformTrimIntegrationTest : public VulkanRecordingIntegrationTest
{
protected:
    static constexpr uint32_t blockSize = 1024;
    VulkanUniformBufferAllocator* allocator{};

    void SetUp() override
    {
        VulkanRecordingIntegrationTest::SetUp();
        allocator = session->rhi.GetUniformBufferAllocator();
        allocator->Destroy();
        allocator->Init(2, blockSize, 1);
    }

    void Fill(uint32_t count)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            auto allocation = allocator->Alloc(blockSize);
            ASSERT_TRUE(allocation.IsValid());
            std::memset(allocation.pMapped, 0xCD, allocation.size);
        }
    }

    void LowDemand(uint32_t reuses)
    {
        for (uint32_t i = 0; i < reuses; ++i)
        {
            Fill(1);
            allocator->BeginFrame(0);
        }
    }

    VulkanShader* BindCompute()
    {
        auto* shader = Shader("recording_uniform.comp.spv", true);
        RHIComputePipelineCreateInfo info{};
        info.pShader   = shader;
        auto* pipeline = session->rhi.CreatePipeline(info);
        pipelines.push_back(pipeline);
        context->RHIBindPipeline(pipeline);
        return shader;
    }

    void Dispatch(VulkanShader* shader, VulkanBuffer* output, uint32_t value)
    {
        const uint32_t values[4]{value, 0, 0, 0};
        RHIBatchedShaderParameters parameters;
        parameters.AddValueParam(*shader->GetSRDByLocation(test::kLocalResourceSet, 0), values,
                                 sizeof(values));
        parameters.AddResourceParam(*shader->GetSRDByLocation(test::kLocalResourceSet, 1), output,
                                    nullptr, 0);
        context->RHISetShaderParameters(parameters);
        context->RHIDispatch(1, 1, 1);
    }

    void ReleaseCachedUniform(VulkanShader* shader)
    {
        auto parameters = Parameters(shader, Buffer());
        context->RHISetShaderParameters(parameters);
    }

    void CheckValue(VulkanBuffer* output, uint32_t value)
    {
        EXPECT_EQ(*reinterpret_cast<const uint32_t*>(output->Map()), value);
        output->Unmap();
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
};

TEST_F(VulkanUniformTrimIntegrationTest, CooldownKeepsSpareAndRegrowsIndependentlyPerSlot)
{
    Fill(9);
    allocator->BeginFrame(1);
    Fill(5);
    allocator->BeginFrame(0);
    LowDemand(allocator->kTrimDelay - 1);
    EXPECT_EQ(allocator->GetAllocatedBlockCount(0), 9u);
    // Another peak restarts the cooldown instead of freeing and recreating blocks.
    Fill(9);
    allocator->BeginFrame(0);
    LowDemand(allocator->kTrimDelay - 1);
    EXPECT_EQ(allocator->GetAllocatedBlockCount(0), 9u);
    LowDemand(1);
    EXPECT_EQ(allocator->GetAllocatedBlockCount(0), 2u);
    EXPECT_EQ(allocator->GetAllocatedBlockCount(1), 5u);
    Fill(9);
    EXPECT_EQ(allocator->GetAllocatedBlockCount(0), 9u);
    allocator->BeginFrame(0);
    // An entirely idle slot eventually retains just one warm block.
    for (uint32_t i = 0; i < allocator->kTrimDelay; ++i)
    {
        allocator->BeginFrame(0);
    }
    EXPECT_EQ(allocator->GetAllocatedBlockCount(0), 1u);
}

TEST_F(VulkanUniformTrimIntegrationTest, UnsubmittedRecordingPreventsTrimmingAndOverwrite)
{
    Fill(3);
    auto retained = allocator->Alloc(blockSize);
    ASSERT_TRUE(retained.IsValid());
    context->RecordUniformBufferBlock(retained.blockId);
    context->RecordUniformBufferBlock(retained.blockId); // Deduplicated within one workload.
    context->GetCommandBuffer();
    *reinterpret_cast<uint32_t*>(retained.pMapped) = 73;
    allocator->BeginFrame(0);
    LowDemand(allocator->kTrimDelay);
    EXPECT_EQ(allocator->GetAllocatedBlockCount(0), 4u);
    Fill(4); // Must skip the pinned fourth block and grow a fifth.
    EXPECT_EQ(allocator->GetAllocatedBlockCount(0), 5u);
    EXPECT_EQ(*reinterpret_cast<const uint32_t*>(retained.pMapped), 73u);
    ZEN_DELETE(context); // Abandon recording without ever assigning a queue serial.
    context = static_cast<FVulkanCommandListContext*>(
        session->rhi.GetCommandContext(RHICommandContextType::eGraphics));
    allocator->BeginFrame(0);
    LowDemand(allocator->kTrimDelay);
    EXPECT_EQ(allocator->GetAllocatedBlockCount(0), 2u);
}

TEST_F(VulkanUniformTrimIntegrationTest, CachedAndUnsubmittedPackedValuesSurviveTrimming)
{
    auto* shader = BindCompute();
    auto* first  = Buffer();
    auto* second = Buffer();
    Fill(3);
    Dispatch(shader, first, 73);
    SubmitAndWait();
    allocator->BeginFrame(0);
    LowDemand(allocator->kTrimDelay);
    EXPECT_EQ(allocator->GetAllocatedBlockCount(0), 2u);
    // Cached CPU bytes survive destruction of the old buffer. Regrow into the
    // same block location, then re-upload the clean value in a new workload.
    Fill(3);
    RHIBatchedShaderParameters parameters;
    parameters.AddResourceParam(*shader->GetSRDByLocation(test::kLocalResourceSet, 1), second,
                                nullptr, 0);
    context->RHISetShaderParameters(parameters);
    context->RHIDispatch(1, 1, 1);
    ReleaseCachedUniform(shader);
    allocator->BeginFrame(0);
    LowDemand(allocator->kTrimDelay);
    EXPECT_EQ(allocator->GetAllocatedBlockCount(0), 4u);
    SubmitAndWait();
    CheckValue(first, 73);
    CheckValue(second, 73);
    allocator->BeginFrame(0);
    EXPECT_EQ(allocator->GetAllocatedBlockCount(0), 2u);
    Dispatch(shader, second, 91);
    SubmitAndWait();
    CheckValue(second, 91);
}

TEST_F(VulkanUniformTrimIntegrationTest, RecycledBlockRefreshesCleanPackedValues)
{
    auto* shader = BindCompute();
    auto* first  = Buffer();
    auto* second = Buffer();
    Dispatch(shader, first, 73);
    SubmitAndWait();
    allocator->BeginFrame(0);
    auto overwrite = allocator->Alloc(16);
    ASSERT_TRUE(overwrite.IsValid());
    std::memset(overwrite.pMapped, 0xCD, overwrite.size);
    RHIBatchedShaderParameters parameters;
    parameters.AddResourceParam(*shader->GetSRDByLocation(test::kLocalResourceSet, 1), second,
                                nullptr, 0);
    context->RHISetShaderParameters(parameters);
    context->RHIDispatch(1, 1, 1);
    SubmitAndWait();
    EXPECT_EQ(allocator->GetAllocatedBlockCount(0), 1u);
    CheckValue(first, 73);
    CheckValue(second, 73);
}

// Gate real GPU work while testing either production completion backend.
class UniformHostGate
{
public:
    explicit UniformHostGate(VulkanDevice* device) : device(device)
    {
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        info.pNext = &type;
        EXPECT_EQ(vkCreateSemaphore(device->GetVkHandle(), &info, nullptr, &semaphore), VK_SUCCESS);
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
        EXPECT_EQ(vkQueueSubmit(device->GetGfxQueue()->GetVkHandle(), 1, &submit, VK_NULL_HANDLE),
                  VK_SUCCESS);
    }
    ~UniformHostGate()
    {
        Open();
        device->WaitForIdle();
        vkDestroySemaphore(device->GetVkHandle(), semaphore, nullptr);
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

class VulkanUniformQueueTrimTest :
    public VulkanUniformTrimIntegrationTest,
    public testing::WithParamInterface<bool>
{
    void SetUp() override
    {
        VulkanUniformTrimIntegrationTest::SetUp();
        ASSERT_TRUE(session->rhi.GetDevice()->SupportsTimelineSemaphore());
        session->rhi.GetDevice()->GetExtensionFlags().hasTimelineSemaphore = GetParam();
    }
};

TEST_P(VulkanUniformQueueTrimTest, PendingGPUReadPinsStorageUntilCompletion)
{
    auto* shader = BindCompute();
    auto* output = Buffer();
    Fill(3);
    Dispatch(shader, output, 73);
    ReleaseCachedUniform(shader);
    UniformHostGate gate(session->rhi.GetDevice());
    Enqueue(context);
    auto* queue     = context->GetQueue();
    uint64_t serial = 0;
    ASSERT_EQ(queue->SubmitPendingWorkloads(serial), RHISubmissionResult::eSuccess);
    EXPECT_FALSE(queue->WaitForCompletion(serial, 0));
    allocator->BeginFrame(0);
    LowDemand(allocator->kTrimDelay);
    EXPECT_EQ(allocator->GetAllocatedBlockCount(0), 4u);
    EXPECT_FALSE(queue->WaitForCompletion(serial, 0));
    gate.Open();
    ASSERT_TRUE(queue->WaitForCompletion(serial, UINT64_MAX));
    SubmitAndWait(); // Make the completed compute write visible to the host.
    CheckValue(output, 73);
    allocator->BeginFrame(0);
    EXPECT_EQ(allocator->GetAllocatedBlockCount(0), 2u);
}

TEST_P(VulkanUniformQueueTrimTest, MergedRejectedWorkloadsPinStorageUntilDiscard)
{
    Fill(3);
    auto allocation = allocator->Alloc(blockSize);
    ASSERT_TRUE(allocation.IsValid());
    // Only the child references the block; merging must preserve its pending count.
    context->GetCommandBuffer();
    Enqueue(context);
    context->RecordUniformBufferBlock(allocation.blockId);
    context->GetCommandBuffer();
    Enqueue(context);
    allocation      = {};
    auto* queue     = context->GetQueue();
    uint64_t serial = 0;
    {
        test::ScopedVulkanCall<PFN_vkQueueSubmit> reject(
            vkQueueSubmit, +[](VkQueue, uint32_t, const VkSubmitInfo*, VkFence) -> VkResult {
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            });
        EXPECT_EQ(queue->SubmitPendingWorkloads(serial), RHISubmissionResult::eRejected);
    }
    allocator->BeginFrame(0);
    LowDemand(allocator->kTrimDelay);
    EXPECT_EQ(allocator->GetAllocatedBlockCount(0), 4u);
    queue->DiscardPendingWorkloads();
    allocator->BeginFrame(0);
    EXPECT_EQ(allocator->GetAllocatedBlockCount(0), 2u);
}

TEST_P(VulkanUniformQueueTrimTest, AcceptedRecordingsReleaseDuplicateBlockCounts)
{
    auto* shader = BindCompute();
    auto* first  = Buffer();
    auto* second = Buffer();
    Fill(3);
    Dispatch(shader, first, 73);
    Enqueue(context);
    RHIBatchedShaderParameters parameters;
    parameters.AddResourceParam(*shader->GetSRDByLocation(test::kLocalResourceSet, 1), second,
                                nullptr, 0);
    context->RHISetShaderParameters(parameters);
    context->RHIDispatch(1, 1, 1); // Same block in another recording (merged in timeline mode).
    Enqueue(context);
    ReleaseCachedUniform(shader);
    UniformHostGate gate(session->rhi.GetDevice());
    auto* queue     = context->GetQueue();
    uint64_t serial = 0;
    ASSERT_EQ(queue->SubmitPendingWorkloads(serial), RHISubmissionResult::eSuccess);
    EXPECT_FALSE(queue->WaitForCompletion(serial, 0));
    allocator->BeginFrame(0);
    LowDemand(allocator->kTrimDelay);
    EXPECT_EQ(allocator->GetAllocatedBlockCount(0), 4u);
    gate.Open();
    ASSERT_TRUE(queue->WaitForCompletion(serial, UINT64_MAX));
    SubmitAndWait();
    CheckValue(first, 73);
    CheckValue(second, 73);
    allocator->BeginFrame(0);
    EXPECT_EQ(allocator->GetAllocatedBlockCount(0), 2u);
}

TEST_P(VulkanUniformQueueTrimTest, EveryQueueMustCompleteItsOwnSerial)
{
    auto destroyContext = [](FVulkanCommandListContext* value) {
        ZEN_DELETE(value);
    };
    auto compute = std::unique_ptr<FVulkanCommandListContext, decltype(destroyContext)>(
        static_cast<FVulkanCommandListContext*>(
            session->rhi.GetCommandContext(RHICommandContextType::eAsyncCompute)),
        destroyContext);
    auto* graphicsQueue = context->GetQueue();
    auto* computeQueue  = compute->GetQueue();
    if (graphicsQueue->GetVkHandle() == computeQueue->GetVkHandle())
    {
        GTEST_SKIP() << "Requires distinct native graphics and compute queues";
    }
    Fill(3);
    auto allocation = allocator->Alloc(blockSize);
    ASSERT_TRUE(allocation.IsValid());
    context->RecordUniformBufferBlock(allocation.blockId);
    context->GetCommandBuffer();
    Enqueue(context);
    UniformHostGate gate(session->rhi.GetDevice());
    uint64_t graphicsSerial = 0;
    ASSERT_EQ(graphicsQueue->SubmitPendingWorkloads(graphicsSerial), RHISubmissionResult::eSuccess);
    // Higher completed serials on compute cannot satisfy an earlier graphics serial.
    for (uint32_t i = 0; i < 3; ++i)
    {
        compute->RecordUniformBufferBlock(allocation.blockId);
        compute->GetCommandBuffer();
        Enqueue(compute.get());
        uint64_t computeSerial = 0;
        ASSERT_EQ(computeQueue->SubmitPendingWorkloads(computeSerial),
                  RHISubmissionResult::eSuccess);
        ASSERT_TRUE(computeQueue->WaitForCompletion(computeSerial, UINT64_MAX));
    }
    ASSERT_GT(computeQueue->GetLastCompletedSerial(), graphicsSerial);
    EXPECT_FALSE(graphicsQueue->WaitForCompletion(graphicsSerial, 0));
    allocator->BeginFrame(0);
    LowDemand(allocator->kTrimDelay);
    EXPECT_EQ(allocator->GetAllocatedBlockCount(0), 4u);
    gate.Open();
    ASSERT_TRUE(graphicsQueue->WaitForCompletion(graphicsSerial, UINT64_MAX));
    allocator->BeginFrame(0);
    EXPECT_EQ(allocator->GetAllocatedBlockCount(0), 2u);
}

TEST_P(VulkanUniformQueueTrimTest, RejectedSubmissionTransfersCountsOnlyOnSuccessfulRetry)
{
    auto* shader = BindCompute();
    auto* output = Buffer();
    Fill(3);
    Dispatch(shader, output, 73);
    ReleaseCachedUniform(shader);
    Enqueue(context);
    auto* queue     = context->GetQueue();
    uint64_t serial = 0;
    {
        test::ScopedVulkanCall<PFN_vkQueueSubmit> reject(
            vkQueueSubmit, +[](VkQueue, uint32_t, const VkSubmitInfo*, VkFence) -> VkResult {
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            });
        EXPECT_EQ(queue->SubmitPendingWorkloads(serial), RHISubmissionResult::eRejected);
    }
    EXPECT_EQ(serial, 0u);
    allocator->BeginFrame(0);
    LowDemand(allocator->kTrimDelay);
    EXPECT_EQ(allocator->GetAllocatedBlockCount(0), 4u);
    UniformHostGate gate(session->rhi.GetDevice());
    ASSERT_EQ(queue->SubmitPendingWorkloads(serial), RHISubmissionResult::eSuccess);
    EXPECT_FALSE(queue->WaitForCompletion(serial, 0));
    allocator->BeginFrame(0);
    EXPECT_EQ(allocator->GetAllocatedBlockCount(0), 4u);
    gate.Open();
    ASSERT_TRUE(queue->WaitForCompletion(serial, UINT64_MAX));
    SubmitAndWait();
    CheckValue(output, 73);
    allocator->BeginFrame(0);
    EXPECT_EQ(allocator->GetAllocatedBlockCount(0), 2u);
}

TEST_P(VulkanUniformQueueTrimTest, UncertainSubmissionRemainsProtectedAfterUnrelatedCompletion)
{
    Fill(3);
    auto allocation = allocator->Alloc(blockSize);
    ASSERT_TRUE(allocation.IsValid());
    context->RecordUniformBufferBlock(allocation.blockId);
    context->GetCommandBuffer();
    Enqueue(context);
    auto* queue     = context->GetQueue();
    uint64_t serial = 0;
    {
        test::ScopedVulkanCall<PFN_vkQueueSubmit> fail(
            vkQueueSubmit, +[](VkQueue, uint32_t, const VkSubmitInfo*, VkFence) -> VkResult {
                return VK_ERROR_DEVICE_LOST;
            });
        EXPECT_EQ(queue->SubmitPendingWorkloads(serial), RHISubmissionResult::eFatal);
    }
    queue->DiscardPendingWorkloads(true);
    // The injected failure never reached the driver. Complete unrelated native work
    // with the serial that the failed batch would have used, without unpinning it.
    context->GetCommandBuffer();
    Enqueue(context);
    ASSERT_EQ(queue->SubmitPendingWorkloads(serial), RHISubmissionResult::eSuccess);
    ASSERT_TRUE(queue->WaitForCompletion(serial, UINT64_MAX));
    allocator->BeginFrame(0);
    LowDemand(allocator->kTrimDelay);
    EXPECT_EQ(allocator->GetAllocatedBlockCount(0), 4u);
    // Uncertain recordings are retained until teardown, which also exercises cleanup
    // after the allocator has been destroyed and the queue still has abandoned work.
}

INSTANTIATE_TEST_SUITE_P(TimelineAndFence, VulkanUniformQueueTrimTest, testing::Bool());

TEST_F(VulkanRecordingIntegrationTest, MeasureRepeatedDrawRecording)
{
    auto* target   = Texture();
    auto layout    = Layout(target);
    auto* shader   = Shader();
    auto* pipeline = Graphics(PipelineInfo(shader, layout));
    auto* uniform  = Buffer();
    Uniform(uniform, 0, Color(1, 0, 0, 1));
    auto parameters = Parameters(shader, uniform);
    context->RHIBindPipeline(pipeline);
    context->RHISetShaderParameters(parameters);
    context->RHIBindVertexBuffer(uniform, 0);
    HeapVector<double> times;
    HeapVector<VkCommandBuffer> recordings;
    bool reusedHandle                 = false;
    constexpr uint32_t drawsPerSample = 1000;
    for (uint32_t sample = 0; sample < 8; ++sample)
    {
        Transition(target,
                   sample == 0 ? VK_IMAGE_LAYOUT_UNDEFINED :
                                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        context->RHIBeginRendering(&layout);
        const auto commands = Commands();
        reusedHandle |=
            std::find(recordings.begin(), recordings.end(), commands) != recordings.end();
        recordings.push_back(commands);
        ResetCounts();
        const auto start = Clock::now();
        for (uint32_t i = 0; i < drawsPerSample; ++i)
        {
            DynamicState();
            context->RHIBindPipeline(pipeline);
            context->RHIDraw(3, 1, 0, 0);
        }
        const double micros =
            std::chrono::duration<double, std::micro>(Clock::now() - start).count();
        if (sample != 0)
        {
            times.push_back(micros);
        }
        EXPECT_EQ(draw->calls, drawsPerSample);
        EXPECT_EQ(bindPipeline->calls, 1u);
        EXPECT_EQ(bindDescriptors->calls, 1u);
        EXPECT_EQ(viewport->calls, 1u);
        EXPECT_EQ(scissor->calls, 1u);
        EXPECT_EQ(depthBias->calls, 1u);
        EXPECT_EQ(lineWidth->calls, 1u);
        EXPECT_EQ(blendConstants->calls, 0u);
        EXPECT_EQ(vertices->calls, 1u);
        context->RHIEndRendering();
        SubmitAndWait();
    }
    std::sort(times.begin(), times.end());
    EXPECT_TRUE(reusedHandle);
    std::printf(
        "RHI_BENCHMARK draws=%u median_us=%.3f min_us=%.3f max_us=%.3f pipeline=%u descriptors=%u updates=%u viewport=%u scissor=%u depth_bias=%u line_width=%u blend_constants=%u vertices=%u\n",
        drawsPerSample, times[times.size() / 2], times[0], times.back(), bindPipeline->calls,
        bindDescriptors->calls, updateDescriptors->calls, viewport->calls, scissor->calls,
        depthBias->calls, lineWidth->calls, blendConstants->calls, vertices->calls);
    auto* readback = Copy(target);
    SubmitAndWait();
    CheckPixel(readback, 4, 4, Color(1, 0, 0, 1));
}

TEST_F(VulkanRecordingIntegrationTest, ChangedDescriptorOffsetsResourcesAndScissorsReachTheGPU)
{
    auto* target          = Texture();
    auto layout           = Layout(target);
    auto* shader          = Shader();
    auto* pipeline        = Graphics(PipelineInfo(shader, layout));
    auto* first           = Buffer();
    auto* second          = Buffer();
    const uint32_t offset = static_cast<uint32_t>(session->rhi.GetDevice()
                                                      ->GetPhysicalDeviceProperties()
                                                      .limits.minUniformBufferOffsetAlignment);
    Uniform(first, 0, Color(1, 0, 0, 1));
    Uniform(first, offset, Color(0, 1, 0, 1));
    Uniform(second, 0, Color(0, 0, 1, 1));
    Transition(target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    context->RHIBeginRendering(&layout);
    context->RHIBindPipeline(pipeline);
    DynamicState();
    ResetCounts();
    for (uint32_t region = 0; region < 4; ++region)
    {
        context->RHISetShaderParameters(
            Parameters(shader, region == 2 ? second : first, region == 1 ? offset : 0));
        context->RHISetScissor(region * 2, 0, region * 2 + 2, 8);
        context->RHIDraw(3, 1, 0, 0);
    }
    EXPECT_EQ(bindPipeline->calls, 1u);
    EXPECT_EQ(bindDescriptors->calls, 4u);
    EXPECT_EQ(updateDescriptors->calls, 2u);
    EXPECT_EQ(viewport->calls, 1u);
    EXPECT_EQ(scissor->calls, 4u);
    context->RHIEndRendering();
    auto* readback = Copy(target);
    SubmitAndWait();
    CheckPixel(readback, 0, 4, Color(1, 0, 0, 1));
    CheckPixel(readback, 2, 4, Color(0, 1, 0, 1));
    CheckPixel(readback, 4, 4, Color(0, 0, 1, 1));
    CheckPixel(readback, 6, 4, Color(1, 0, 0, 1));
}

TEST_F(VulkanRecordingIntegrationTest, StaticPipelineSwitchRestoresDynamicValuesAndDescriptorLayout)
{
    auto* target          = Texture();
    auto layout           = Layout(target);
    auto* dynamicShader   = Shader();
    auto* staticShader    = Shader();
    auto* dynamicPipeline = Graphics(PipelineInfo(dynamicShader, layout));
    auto staticLayout     = layout;
    staticLayout.SetRenderArea(0, 0, 4, 8);
    auto* staticPipeline = Graphics(PipelineInfo(staticShader, staticLayout, false));
    auto* red            = Buffer();
    auto* green          = Buffer();
    auto* blue           = Buffer();
    Uniform(red, 0, Color(1, 0, 0, 1));
    Uniform(green, 0, Color(0, 1, 0, 1));
    Uniform(blue, 0, Color(0, 0, 1, 1));
    Transition(target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    context->RHIBeginRendering(&layout);
    DynamicState();
    ResetCounts();
    context->RHIBindPipeline(dynamicPipeline);
    context->RHISetShaderParameters(Parameters(dynamicShader, red));
    context->RHIDraw(3, 1, 0, 0);
    context->RHIBindPipeline(staticPipeline);
    context->RHISetShaderParameters(Parameters(staticShader, green));
    context->RHIDraw(3, 1, 0, 0);
    context->RHIBindPipeline(dynamicPipeline);
    context->RHISetShaderParameters(Parameters(dynamicShader, blue));
    context->RHISetScissor(4, 0, 8, 8);
    context->RHIDraw(3, 1, 0, 0);
    EXPECT_EQ(bindPipeline->calls, 3u);
    EXPECT_EQ(bindDescriptors->calls, 3u);
    EXPECT_EQ(viewport->calls, 2u);
    EXPECT_EQ(scissor->calls, 2u);
    EXPECT_EQ(depthBias->calls, 2u);
    EXPECT_EQ(lineWidth->calls, 2u);
    context->RHIEndRendering();
    auto* readback = Copy(target);
    SubmitAndWait();
    CheckPixel(readback, 1, 4, Color(0, 1, 0, 1));
    CheckPixel(readback, 6, 4, Color(0, 0, 1, 1));
}

TEST_F(VulkanRecordingIntegrationTest, GraphicsAndComputeKeepIndependentBindings)
{
    auto* target        = Texture();
    auto layout         = Layout(target);
    auto* shader        = Shader();
    auto* graphics      = Graphics(PipelineInfo(shader, layout));
    auto* computeShader = Shader("descriptor_storage.comp.spv", true);
    auto* compute       = session->rhi.CreatePipeline(RHIComputePipelineCreateInfo{computeShader});
    pipelines.push_back(compute);
    auto* uniform = Buffer();
    auto* output  = Buffer();
    Uniform(uniform, 0, Color(1, 0, 0, 1));
    Transition(target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    context->RHIBeginRendering(&layout);
    context->RHIBindPipeline(graphics);
    context->RHISetShaderParameters(Parameters(shader, uniform));
    DynamicState();
    ResetCounts();
    context->RHIDraw(3, 1, 0, 0);
    context->RHIEndRendering();
    context->RHIBindPipeline(compute);
    context->RHISetShaderParameters(Parameters(computeShader, output));
    context->RHIDispatch(1, 1, 1);
    VkMemoryBarrier dependency{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    dependency.srcAccessMask = dependency.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(Commands(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &dependency, 0, nullptr, 0,
                         nullptr);
    context->RHIBindPipeline(compute);
    context->RHIDispatch(1, 1, 1);
    Transition(target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    context->RHIBeginRendering(&layout);
    context->RHIBindPipeline(graphics);
    context->RHIDraw(3, 1, 0, 0);
    context->RHIEndRendering();
    EXPECT_EQ(bindPipeline->calls, 2u);
    EXPECT_EQ(bindDescriptors->calls, 2u);
    EXPECT_EQ(viewport->calls, 1u);
    auto* readback = Copy(target);
    SubmitAndWait();
    CheckPixel(readback, 4, 4, Color(1, 0, 0, 1));
    EXPECT_EQ(*reinterpret_cast<const uint32_t*>(output->Map()), 0x1234u);
    output->Unmap();
}

TEST_F(VulkanRecordingIntegrationTest, ChangedVertexBufferAndOffsetSelectDifferentGeometry)
{
    auto* target     = Texture();
    auto layout      = Layout(target);
    auto* shader     = Shader("recording_uniform.frag.spv", false, "recording_vertex.vert.spv");
    auto* pipeline   = Graphics(PipelineInfo(shader, layout));
    auto* vertexData = Buffer();
    auto* alternateVertexData = Buffer();
    const float positions[]   = {-1, -1, 0, -1, 0, 1, -1, -1, 0, 1, -1, 1,
                                 0,  -1, 1, -1, 1, 1, 0,  -1, 1, 1, 0,  1};
    memcpy(vertexData->Map(), positions, sizeof(positions));
    vertexData->Unmap();
    memcpy(alternateVertexData->Map(), positions, sizeof(positions) / 2);
    alternateVertexData->Unmap();
    auto* red   = Buffer();
    auto* green = Buffer();
    auto* blue  = Buffer();
    Uniform(red, 0, Color(1, 0, 0, 1));
    Uniform(green, 0, Color(0, 1, 0, 1));
    Uniform(blue, 0, Color(0, 0, 1, 1));
    Transition(target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    context->RHIBeginRendering(&layout);
    context->RHIBindPipeline(pipeline);
    DynamicState();
    ResetCounts();
    context->RHISetShaderParameters(Parameters(shader, red));
    context->RHIBindVertexBuffer(vertexData, 0);
    context->RHIDraw(6, 1, 0, 0);
    context->RHISetShaderParameters(Parameters(shader, green));
    context->RHIBindVertexBuffer(vertexData, sizeof(positions) / 2);
    context->RHIDraw(6, 1, 0, 0);
    context->RHISetShaderParameters(Parameters(shader, blue));
    context->RHIBindVertexBuffer(alternateVertexData, 0);
    context->RHIDraw(6, 1, 0, 0);
    EXPECT_EQ(vertices->calls, 3u);
    EXPECT_EQ(bindPipeline->calls, 1u);
    context->RHIEndRendering();
    auto* readback = Copy(target);
    SubmitAndWait();
    CheckPixel(readback, 1, 4, Color(0, 0, 1, 1));
    CheckPixel(readback, 6, 4, Color(0, 1, 0, 1));
}

TEST_F(VulkanRecordingIntegrationTest, ExternalNativeStateCanBeInvalidatedBeforeResumingRHI)
{
    auto* target           = Texture();
    auto layout            = Layout(target);
    auto* shader           = Shader();
    auto* pipeline         = Graphics(PipelineInfo(shader, layout));
    auto* externalPipeline = Graphics(PipelineInfo(Shader("pipeline.frag.spv"), layout, false));
    auto* uniform          = Buffer();
    Uniform(uniform, 0, Color(0, 1, 0, 1));
    Transition(target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    context->RHIBeginRendering(&layout);
    context->RHIBindPipeline(pipeline);
    context->RHISetShaderParameters(Parameters(shader, uniform));
    DynamicState();
    ResetCounts();
    context->RHIDraw(3, 1, 0, 0);
    vkCmdBindPipeline(Commands(), VK_PIPELINE_BIND_POINT_GRAPHICS,
                      static_cast<VulkanPipeline*>(externalPipeline)->GetVkPipeline());
    VkViewport nativeViewport{0, 0, 1, 1, 0, 1};
    vkCmdSetViewport(Commands(), 0, 1, &nativeViewport);
    context->GetCommandBuffer()->InvalidateCachedState();
    context->RHIDraw(3, 1, 0, 0);
    EXPECT_EQ(bindPipeline->calls, 3u);
    EXPECT_EQ(bindDescriptors->calls, 2u);
    EXPECT_EQ(viewport->calls, 3u);
    context->RHIEndRendering();
    auto* readback = Copy(target);
    SubmitAndWait();
    CheckPixel(readback, 7, 7, Color(0, 1, 0, 1));
}

struct PipelineCacheProbe
{
    static inline PFN_vkCreateGraphicsPipelines original;
    static inline PFN_vkCreateComputePipelines originalCompute;
    static inline VkPipelineCache cache;
    static VKAPI_ATTR VkResult VKAPI_CALL Create(VkDevice device,
                                                 VkPipelineCache suppliedCache,
                                                 uint32_t count,
                                                 const VkGraphicsPipelineCreateInfo* infos,
                                                 const VkAllocationCallbacks* allocator,
                                                 VkPipeline* pipelines)
    {
        EXPECT_EQ(suppliedCache, GVulkanRHI->GetDevice()->GetPipelineCache());
        return original(device, cache, count, infos, allocator, pipelines);
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Compute(VkDevice device,
                                                  VkPipelineCache suppliedCache,
                                                  uint32_t count,
                                                  const VkComputePipelineCreateInfo* infos,
                                                  const VkAllocationCallbacks* allocator,
                                                  VkPipeline* pipelines)
    {
        EXPECT_EQ(suppliedCache, GVulkanRHI->GetDevice()->GetPipelineCache());
        return originalCompute(device, suppliedCache, count, infos, allocator, pipelines);
    }
};

TEST_F(VulkanRecordingIntegrationTest, DeviceCacheIsSharedByGraphicsAndComputeCreation)
{
    const VkPipelineCache cache = session->rhi.GetDevice()->GetPipelineCache();
    ASSERT_NE(cache, VK_NULL_HANDLE);
    PipelineCacheProbe::cache           = cache;
    PipelineCacheProbe::original        = vkCreateGraphicsPipelines;
    PipelineCacheProbe::originalCompute = vkCreateComputePipelines;
    test::ScopedVulkanCall<PFN_vkCreateGraphicsPipelines> graphics(vkCreateGraphicsPipelines,
                                                                   PipelineCacheProbe::Create);
    test::ScopedVulkanCall<PFN_vkCreateComputePipelines> compute(vkCreateComputePipelines,
                                                                 PipelineCacheProbe::Compute);
    auto layout = Layout(Texture());
    Graphics(PipelineInfo(Shader(), layout));
    pipelines.push_back(session->rhi.CreatePipeline(
        RHIComputePipelineCreateInfo{Shader("descriptor_storage.comp.spv", true)}));
    size_t size = 0;
    EXPECT_EQ(vkGetPipelineCacheData(session->rhi.GetVkDevice(), cache, &size, nullptr),
              VK_SUCCESS);
    EXPECT_GE(size, sizeof(VkPipelineCacheHeaderVersionOne));
    PipelineCacheProbe::cache = VK_NULL_HANDLE;
}

TEST_F(VulkanRecordingIntegrationTest, MeasureNativePipelineCacheBenefit)
{
    auto layout                  = Layout(Texture());
    auto* shader                 = Shader();
    PipelineCacheProbe::original = vkCreateGraphicsPipelines;
    PipelineCacheProbe::cache    = VK_NULL_HANDLE;
    test::ScopedVulkanCall<PFN_vkCreateGraphicsPipelines> probe(vkCreateGraphicsPipelines,
                                                                PipelineCacheProbe::Create);
    VkPipelineCacheCreateInfo cacheInfo{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    VkPipelineCache cache{};
    ASSERT_EQ(vkCreatePipelineCache(session->rhi.GetVkDevice(), &cacheInfo, nullptr, &cache),
              VK_SUCCESS);
    HeapVector<double> uncachedTimes, cachedTimes;
    // Warm each variant before alternating measured runs, excluding first-use shader work.
    for (uint32_t round = 0; round < 8; ++round)
    {
        for (uint32_t mode = 0; mode < 2; ++mode)
        {
            const bool cached         = (mode ^ (round & 1)) != 0;
            PipelineCacheProbe::cache = cached ? cache : VK_NULL_HANDLE;
            double micros             = 0;
            for (uint32_t variant = 0; variant < 32; ++variant)
            {
                auto info                                    = PipelineInfo(shader, layout, false);
                info.states.colorBlendState.blendConstants.r = float(variant) / 32;
                const auto start                             = Clock::now();
                auto* pipeline                               = session->rhi.CreatePipeline(info);
                micros += std::chrono::duration<double, std::micro>(Clock::now() - start).count();
                session->rhi.DestroyPipeline(pipeline);
            }
            if (round != 0)
            {
                (cached ? cachedTimes : uncachedTimes).push_back(micros);
            }
        }
    }
    size_t size = 0;
    ASSERT_EQ(vkGetPipelineCacheData(session->rhi.GetVkDevice(), cache, &size, nullptr),
              VK_SUCCESS);
    std::sort(uncachedTimes.begin(), uncachedTimes.end());
    std::sort(cachedTimes.begin(), cachedTimes.end());
    std::printf(
        "RHI_BENCHMARK pipelines=32 null_cache_median_us=%.3f cache_median_us=%.3f cache_bytes=%zu\n",
        uncachedTimes[uncachedTimes.size() / 2], cachedTimes[cachedTimes.size() / 2], size);
    vkDestroyPipelineCache(session->rhi.GetVkDevice(), cache, nullptr);
    PipelineCacheProbe::cache = VK_NULL_HANDLE;
}
} // namespace
