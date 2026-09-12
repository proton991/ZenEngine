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
        parameters.AddResourceParam(*shader->GetSRDByLocation(0, 0), buffer, nullptr, 0, offset);
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
        ASSERT_TRUE(queue->WaitForSubmission(serial, UINT64_MAX));
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
