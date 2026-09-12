#include "VulkanIntegrationFixture.h"
#include "ScopedVulkanCall.h"
#include "Graphics/VulkanRHI/VulkanBuffer.h"
#include "Graphics/VulkanRHI/VulkanCommandList.h"
#include "Graphics/VulkanRHI/VulkanDescriptorState.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanPipeline.h"
#include "Graphics/VulkanRHI/VulkanTexture.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <memory>
#include <unordered_set>
#include <vector>

namespace
{
using namespace zen;

struct BindingObserver
{
    static inline PFN_vkCmdBindDescriptorSets bind;
    static inline uint32_t firstSet;
    static inline std::vector<VkDescriptorSet> sets;
    static inline std::vector<uint32_t> offsets;

    static VKAPI_ATTR void VKAPI_CALL Bind(VkCommandBuffer commands,
                                           VkPipelineBindPoint point,
                                           VkPipelineLayout layout,
                                           uint32_t first,
                                           uint32_t count,
                                           const VkDescriptorSet* descriptors,
                                           uint32_t offsetCount,
                                           const uint32_t* dynamicOffsets)
    {
        firstSet = first;
        sets.assign(descriptors, descriptors + count);
        offsets.clear();
        if (offsetCount != 0)
        {
            offsets.assign(dynamicOffsets, dynamicOffsets + offsetCount);
        }
        bind(commands, point, layout, first, count, descriptors, offsetCount, dynamicOffsets);
    }

    static VKAPI_ATTR VkBool32 VKAPI_CALL
    Validation(VkDebugUtilsMessageSeverityFlagBitsEXT,
               VkDebugUtilsMessageTypeFlagsEXT,
               const VkDebugUtilsMessengerCallbackDataEXT* data,
               void*)
    {
        ADD_FAILURE() << data->pMessage;
        return VK_FALSE;
    }
};

class VulkanBindingIntegrationTest : public testing::Test
{
protected:
    std::unique_ptr<test::VulkanSession> session;
    std::unique_ptr<test::ScopedVulkanCall<PFN_vkCmdBindDescriptorSets>> observe;
    FVulkanCommandListContext* context{};
    RHICommandList* commandList{};
    VkDebugUtilsMessengerEXT messenger{};
    std::vector<RHIBuffer*> buffers;
    std::vector<RHITexture*> textures;
    std::vector<RHISampler*> samplers;
    std::vector<RHIShader*> shaders;
    std::vector<RHIPipeline*> pipelines;

    void SetUp() override
    {
        session               = std::make_unique<test::VulkanSession>();
        BindingObserver::bind = vkCmdBindDescriptorSets;
        BindingObserver::sets.clear();
        BindingObserver::offsets.clear();
        BindingObserver::firstSet = UINT32_MAX;
        observe = std::make_unique<test::ScopedVulkanCall<PFN_vkCmdBindDescriptorSets>>(
            vkCmdBindDescriptorSets, BindingObserver::Bind);
        VkDebugUtilsMessengerCreateInfoEXT info{
            VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        info.pfnUserCallback = BindingObserver::Validation;
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
        observe.reset();
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

    void InitializeRed(VulkanTexture* texture)
    {
        Transition(texture, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);
        const VkClearColorValue red{{1, 0, 0, 1}};
        const auto range = texture->GetVkSubresourceRange();
        vkCmdClearColorImage(Commands(), texture->GetVkImage(),
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &red, 1, &range);
        Transition(texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
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

TEST_F(VulkanBindingIntegrationTest, UniformArraysEmitAllZeroOffsetsByDefault)
{
    auto* pipeline = Compute("binding_uniform_array.comp.spv");
    auto* output   = Buffer();
    RHIBatchedShaderParameters parameters;
    for (uint32_t i = 0; i < 4; ++i)
    {
        auto* buffer                                = Buffer(16, true);
        *reinterpret_cast<uint32_t*>(buffer->Map()) = 100 + i;
        buffer->Unmap();
        parameters.AddResourceParam(*pipeline->GetShader()->GetSRDByLocation(0, i == 0 ? 1 : 3),
                                    buffer, nullptr, i == 0 ? 0 : i - 1);
    }
    parameters.AddResourceParam(*pipeline->GetShader()->GetSRDByLocation(0, 7), output, nullptr, 0);
    context->RHISetShaderParameters(parameters);
    context->RHIDispatch(1, 1, 1);
    EXPECT_EQ(BindingObserver::offsets, (std::vector<uint32_t>{0, 0, 0, 0}));
    SubmitAndWait();
    const auto* values = reinterpret_cast<const uint32_t*>(output->Map());
    for (uint32_t i = 0; i < 4; ++i)
    {
        EXPECT_EQ(values[i], 100 + i);
    }
    output->Unmap();
}

TEST_F(VulkanBindingIntegrationTest, UniformElementOffsetsFollowBindingOrderAndReuseDescriptors)
{
    auto* pipeline = Compute("binding_uniform_array.comp.spv");
    const uint32_t alignment =
        static_cast<uint32_t>(std::max<VkDeviceSize>(16,
                                                     session->rhi.GetDevice()
                                                         ->GetPhysicalDeviceProperties()
                                                         .limits.minUniformBufferOffsetAlignment));
    auto* uniform = Buffer(alignment * 4, true);
    auto* output  = Buffer();
    auto* bytes   = uniform->Map();
    for (uint32_t i = 0; i < 4; ++i)
    {
        *reinterpret_cast<uint32_t*>(bytes + i * alignment) = 10 + i;
    }
    uniform->Unmap();
    RHIBatchedShaderParameters parameters;
    const auto& array = *pipeline->GetShader()->GetSRDByLocation(0, 3);
    parameters.AddResourceParam(array, uniform, nullptr, 2, alignment * 2);
    parameters.AddResourceParam(array, uniform, nullptr, 0, alignment);
    parameters.AddResourceParam(array, uniform, nullptr, 1);
    parameters.AddResourceParam(*pipeline->GetShader()->GetSRDByLocation(0, 1), uniform, nullptr, 0,
                                alignment * 3);
    parameters.AddResourceParam(*pipeline->GetShader()->GetSRDByLocation(0, 7), output, nullptr, 0);

    VulkanDescriptorSetState state;
    state.SetPipeline(static_cast<VulkanPipeline*>(pipeline));
    state.SetShaderParameters(parameters);
    HeapVector<VkDescriptorSet> sets;
    HeapVector<uint32_t> offsets;
    uint32_t first = 0;
    state.FlushPendingDescriptorWrites(context, sets, first, offsets);
    ASSERT_EQ(sets.size(), 1u);
    const auto original = sets[0];
    RHIBatchedShaderParameters changed;
    changed.AddResourceParam(array, uniform, nullptr, 1, alignment * 2);
    state.SetShaderParameters(changed);
    state.FlushPendingDescriptorWrites(context, sets, first, offsets);
    ASSERT_EQ(sets.size(), 1u);
    EXPECT_EQ(sets[0], original);
    EXPECT_EQ((std::vector<uint32_t>(offsets.begin(), offsets.end())),
              (std::vector<uint32_t>{alignment * 3, alignment, alignment * 2, alignment * 2}));

    context->RHISetShaderParameters(parameters);
    context->RHISetShaderParameters(changed);
    context->RHIDispatch(1, 1, 1);
    EXPECT_EQ(BindingObserver::sets[0], original);
    SubmitAndWait();
    const auto* values = reinterpret_cast<const uint32_t*>(output->Map());
    EXPECT_EQ((std::vector<uint32_t>(values, values + 4)), (std::vector<uint32_t>{13, 11, 12, 12}));
    output->Unmap();
}

TEST_F(VulkanBindingIntegrationTest, GlobalBindlessSetBindsWithoutLocalParameters)
{
    auto* manager = session->rhi.GetBindlessDescriptorPoolManager();
    if (manager->GetGlobalBindlessSet() == VK_NULL_HANDLE)
    {
        GTEST_SKIP() << "Bindless heaps unavailable";
    }
    auto* source  = Texture();
    auto* sampler = Sampler();
    ASSERT_TRUE(manager->RegisterBindlessResource(source, 0));
    ASSERT_TRUE(manager->RegisterBindlessResource(sampler, 0));
    InitializeRed(source);
    auto* target = Texture(RHITextureUsageFlagBits::eColorAttachment);
    auto* output = Buffer(64);
    RHIRenderingLayout layout{};
    layout.SetRenderArea(0, 0, 4, 4);
    layout.AddColorRenderTarget(DataFormat::eR8G8B8A8UNORM, target, RHIRenderTargetLoadOp::eClear,
                                RHIRenderTargetStoreOp::eStore);
    RHIGfxPipelineCreateInfo info{};
    info.pShader = Shader("binding_bindless.frag.spv", true);
    ASSERT_TRUE(static_cast<VulkanShader*>(info.pShader)->HasGlobalBindlessSet());
    info.pRenderingLayout = &layout;
    info.states.colorBlendState.AddAttachment();
    auto* pipeline = session->rhi.CreatePipeline(info);
    pipelines.push_back(pipeline);
    Transition(target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    context->RHIBeginRendering(&layout);
    context->RHIBindPipeline(pipeline);
    PushIndex(pipeline, 0);
    context->RHIDraw(3, 1, 0, 0);
    context->RHIEndRendering();
    EXPECT_EQ(BindingObserver::firstSet, 0u);
    EXPECT_EQ(BindingObserver::sets,
              (std::vector<VkDescriptorSet>{manager->GetGlobalBindlessSet()}));
    Transition(target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
               VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent      = {4, 4, 1};
    vkCmdCopyImageToBuffer(Commands(), target->GetVkImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           output->GetVkBuffer(), 1, &copy);
    SubmitAndWait(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    const auto* pixels = reinterpret_cast<const uint32_t*>(output->Map());
    for (uint32_t i = 0; i < 16; ++i)
    {
        EXPECT_EQ(pixels[i], 0xFF0000FFu);
    }
    output->Unmap();
}

TEST_F(VulkanBindingIntegrationTest, BindlessViewRetainsOwnerAndBindsAlongsideSparseLocalSet)
{
    auto* manager = session->rhi.GetBindlessDescriptorPoolManager();
    if (manager->GetGlobalBindlessSet() == VK_NULL_HANDLE)
    {
        GTEST_SKIP() << "Bindless heaps unavailable";
    }
    auto* pipeline = Compute("binding_bindless.comp.spv");
    auto* texture  = Texture();
    auto* sampler  = Sampler();
    auto* output   = Buffer();
    InitializeRed(texture);
    auto* view = texture->GetDefaultView();
    RHIBatchedShaderParameters parameters;
    parameters.AddResourceParam(*pipeline->GetShader()->GetSRDByLocation(0, 0), view, nullptr, 0);
    parameters.AddResourceParam(*pipeline->GetShader()->GetSRDByLocation(0, 2), sampler, nullptr,
                                0);
    parameters.AddResourceParam(*pipeline->GetShader()->GetSRDByLocation(2, 0), output, nullptr, 0);
    context->RHISetShaderParameters(parameters);
    EXPECT_EQ(texture->GetRefCount(), 2u);
    EXPECT_EQ(view->GetRefCount(), 2u);
    EXPECT_EQ(sampler->GetRefCount(), 2u);
    session->rhi.DestroyTexture(texture);
    textures.back() = nullptr;
    session->rhi.DestroySampler(sampler);
    samplers.back() = nullptr;
    EXPECT_EQ(texture->GetRefCount(), 1u);
    EXPECT_EQ(sampler->GetRefCount(), 1u);
    PushIndex(pipeline, 0);
    context->RHIDispatch(1, 1, 1);
    EXPECT_EQ(BindingObserver::firstSet, 0u);
    ASSERT_EQ(BindingObserver::sets.size(), 3u);
    EXPECT_EQ(BindingObserver::sets[0], manager->GetGlobalBindlessSet());
    EXPECT_NE(BindingObserver::sets[1], VK_NULL_HANDLE);
    EXPECT_NE(BindingObserver::sets[2], VK_NULL_HANDLE);
    SubmitAndWait();
    EXPECT_EQ(*reinterpret_cast<uint32_t*>(output->Map()), 0xFF0000FFu);
    output->Unmap();
}

TEST_F(VulkanBindingIntegrationTest, BindlessSlotsRejectReplacementAndInvalidRegistrations)
{
    auto* manager = session->rhi.GetBindlessDescriptorPoolManager();
    if (manager->GetGlobalBindlessSet() == VK_NULL_HANDLE)
    {
        GTEST_SKIP() << "Bindless heaps unavailable";
    }
    auto* first       = Sampler();
    auto* replacement = Sampler();
    ASSERT_TRUE(manager->RegisterBindlessResource(first, 0));
    EXPECT_TRUE(manager->RegisterBindlessResource(first, 0));
    EXPECT_EQ(first->GetRefCount(), 2u);
    EXPECT_FALSE(manager->RegisterBindlessResource(replacement, 0));
    EXPECT_EQ(replacement->GetRefCount(), 1u);
    manager->Flush();
    EXPECT_FALSE(manager->RegisterBindlessResource(replacement, 0));
    EXPECT_FALSE(manager->RegisterBindlessResource(nullptr, 0));
    EXPECT_FALSE(manager->RegisterBindlessResource(Buffer(), 0));
    EXPECT_FALSE(
        manager->RegisterBindlessResource(Texture(RHITextureUsageFlagBits::eColorAttachment), 0));
    EXPECT_FALSE(manager->RegisterBindlessResource(
        first, GetBindlessHeapCapacity(RHIBindlessHeapType::eSampler)));
    first->BumpGeneration();
    EXPECT_FALSE(manager->RegisterBindlessResource(first, 0));

    auto* pipeline = Compute("binding_bindless.comp.spv");
    RHIBatchedShaderParameters parameters;
    parameters.AddResourceParam(*pipeline->GetShader()->GetSRDByLocation(0, 2), replacement,
                                nullptr, 0);
    EXPECT_THROW(context->RHISetShaderParameters(parameters), std::runtime_error);
}

TEST_F(VulkanBindingIntegrationTest, BindlessHeapExhaustionFailsWithoutReplacingPublishedSlots)
{
    auto* manager = session->rhi.GetBindlessDescriptorPoolManager();
    if (manager->GetGlobalBindlessSet() == VK_NULL_HANDLE)
    {
        GTEST_SKIP() << "Bindless heaps unavailable";
    }
    auto* sampler           = Sampler();
    const uint32_t capacity = GetBindlessHeapCapacity(RHIBindlessHeapType::eSampler);
    for (uint32_t i = 0; i < capacity; ++i)
    {
        ASSERT_TRUE(manager->RegisterBindlessResource(sampler, kInvalidBindlessSlotIndex));
    }
    EXPECT_FALSE(manager->RegisterBindlessResource(sampler, kInvalidBindlessSlotIndex));
    EXPECT_TRUE(manager->RegisterBindlessResource(sampler, capacity - 1));
    EXPECT_EQ(sampler->GetRefCount(), capacity + 1);
    manager->Flush();
}

TEST_F(VulkanBindingIntegrationTest, RecordedPushConstantOffsetPreservesEarlierFields)
{
    auto* pipeline = Compute("binding_push_constants.comp.spv");
    auto* output   = Buffer();
    SetOutput(pipeline, output, 0, 0);
    auto* list               = RHICommandList::Create(context);
    commandList              = list;
    const uint32_t initial[] = {11, 22};
    const uint32_t changed   = 99;
    list->SetPushConstants(pipeline, reinterpret_cast<const uint8_t*>(initial), sizeof(initial), 0);
    list->SetPushConstants(pipeline, reinterpret_cast<const uint8_t*>(&changed), sizeof(changed),
                           4);
    list->Execute();
    list->Reset();
    context->RHIDispatch(1, 1, 1);
    SubmitAndWait();
    const auto* values = reinterpret_cast<const uint32_t*>(output->Map());
    EXPECT_EQ(values[0], 11u);
    EXPECT_EQ(values[1], 99u);
    output->Unmap();
}

TEST_F(VulkanBindingIntegrationTest, NativeResourcesRemainDistinctBeyondAllocatorFirstPage)
{
    std::unordered_set<RHIBuffer*> objects;
    std::unordered_set<VkBuffer> handles;
    std::vector<uint64_t> ids;
    for (uint32_t i = 0; i < 5000; ++i)
    {
        auto* buffer = Buffer();
        ASSERT_TRUE(objects.insert(buffer).second);
        ASSERT_TRUE(handles.insert(buffer->GetVkBuffer()).second);
        ids.push_back(buffer->GetStableId());
        *reinterpret_cast<uint32_t*>(buffer->Map()) = i;
        buffer->Unmap();
    }
    for (uint32_t i = 0; i < buffers.size(); ++i)
    {
        EXPECT_EQ(buffers[i]->GetStableId(), ids[i]);
        EXPECT_EQ(*reinterpret_cast<uint32_t*>(buffers[i]->Map()), i);
        buffers[i]->Unmap();
    }
}

TEST_F(VulkanBindingIntegrationTest, TextureWithHeapAllocatedViewListReleasesAllTrackedMemory)
{
    Shutdown();
    const auto liveUsage = [] {
        testing::internal::CaptureStdout();
        DefaultAllocator::ReportMemUsage();
        const std::string report = testing::internal::GetCapturedStdout();
        const size_t start       = report.find("Current Usage");
        EXPECT_NE(start, std::string::npos);
        return start == std::string::npos ? report :
                                            report.substr(start, report.find('\n', start) - start);
    };
    // The name table lives until process exit. Compare against that baseline and
    // repeat enough times to expose a view-list leak in the formatted memory report.
    const std::string baseline = liveUsage();
    {
        test::VulkanSession local;
        RHITextureCreateInfo textureInfo{};
        textureInfo.type   = RHITextureType::e2D;
        textureInfo.format = DataFormat::eR8G8B8A8UNORM;
        textureInfo.usageFlags.SetFlag(RHITextureUsageFlagBits::eSampled);
        RHITextureViewCreateInfo viewInfo{};
        viewInfo.type   = RHITextureType::e2D;
        viewInfo.format = DataFormat::eR8G8B8A8UNORM;
        for (uint32_t iteration = 0; iteration < 1024; ++iteration)
        {
            auto* texture = local.rhi.CreateTexture(textureInfo);
            for (uint32_t i = 0; i < 9; ++i)
            {
                EXPECT_NE(local.rhi.CreateTextureView(texture, viewInfo), nullptr);
            }
            local.rhi.DestroyTexture(texture);
        }
    }
    EXPECT_EQ(liveUsage(), baseline);
}
} // namespace
