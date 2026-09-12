#include "VulkanIntegrationFixture.h"
#include "ScopedVulkanCall.h"
#include "Graphics/VulkanRHI/VulkanBuffer.h"
#include "Graphics/VulkanRHI/VulkanCommandList.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanTexture.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <memory>

namespace
{
using namespace zen;

struct AttachmentObserver
{
    static inline PFN_vkCmdBeginRenderingKHR begin;
    static inline PFN_vkCreateImageView createView;
    static inline PFN_vkCreateGraphicsPipelines createPipeline;
    static inline VkFormat depthFormat;
    static inline VkFormat stencilFormat;
    static inline uint32_t begins;
    static inline uint32_t viewCreates;
    static inline uint32_t layers;
    static inline VkImageView colorView;
    static inline bool failView;

    static VKAPI_ATTR VkResult VKAPI_CALL CreatePipeline(VkDevice device,
                                                         VkPipelineCache cache,
                                                         uint32_t count,
                                                         const VkGraphicsPipelineCreateInfo* infos,
                                                         const VkAllocationCallbacks* allocator,
                                                         VkPipeline* pipelines)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            const auto* rendering =
                static_cast<const VkPipelineRenderingCreateInfo*>(infos[i].pNext);
            if (rendering != nullptr &&
                rendering->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO)
            {
                depthFormat   = rendering->depthAttachmentFormat;
                stencilFormat = rendering->stencilAttachmentFormat;
            }
        }
        return createPipeline(device, cache, count, infos, allocator, pipelines);
    }

    static VKAPI_ATTR void VKAPI_CALL Begin(VkCommandBuffer commands, const VkRenderingInfo* info)
    {
        ++begins;
        layers = info->layerCount;
        colorView =
            info->colorAttachmentCount ? info->pColorAttachments[0].imageView : VK_NULL_HANDLE;
        begin(commands, info);
    }

    static VKAPI_ATTR VkResult VKAPI_CALL CreateView(VkDevice device,
                                                     const VkImageViewCreateInfo* info,
                                                     const VkAllocationCallbacks* allocator,
                                                     VkImageView* view)
    {
        ++viewCreates;
        if (failView)
        {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        return createView(device, info, allocator, view);
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

class VulkanAttachmentIntegrationTest : public testing::Test
{
protected:
    std::unique_ptr<test::VulkanSession> session;
    std::unique_ptr<test::ScopedVulkanCall<PFN_vkCmdBeginRenderingKHR>> observeBegin;
    std::unique_ptr<test::ScopedVulkanCall<PFN_vkCreateImageView>> observeView;
    std::unique_ptr<test::ScopedVulkanCall<PFN_vkCreateGraphicsPipelines>> observePipeline;
    FVulkanCommandListContext* context{};
    VkDebugUtilsMessengerEXT messenger{};
    HeapVector<RHITexture*> textures;
    HeapVector<RHIBuffer*> buffers;
    HeapVector<RHIShader*> shaders;
    HeapVector<RHIPipeline*> pipelines;

    void SetUp() override
    {
        session = std::make_unique<test::VulkanSession>();
        VkDebugUtilsMessengerCreateInfoEXT info{
            VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        info.pfnUserCallback = AttachmentObserver::Validation;
        ASSERT_EQ(
            vkCreateDebugUtilsMessengerEXT(session->rhi.GetInstance(), &info, nullptr, &messenger),
            VK_SUCCESS);
        AttachmentObserver::begin          = vkCmdBeginRenderingKHR;
        AttachmentObserver::createView     = vkCreateImageView;
        AttachmentObserver::createPipeline = vkCreateGraphicsPipelines;
        AttachmentObserver::begins = AttachmentObserver::viewCreates = 0;
        AttachmentObserver::failView                                 = false;
        observeBegin = std::make_unique<test::ScopedVulkanCall<PFN_vkCmdBeginRenderingKHR>>(
            vkCmdBeginRenderingKHR, AttachmentObserver::Begin);
        observeView = std::make_unique<test::ScopedVulkanCall<PFN_vkCreateImageView>>(
            vkCreateImageView, AttachmentObserver::CreateView);
        context = static_cast<FVulkanCommandListContext*>(
            session->rhi.GetCommandContext(RHICommandContextType::eGraphics));
        observePipeline = std::make_unique<test::ScopedVulkanCall<PFN_vkCreateGraphicsPipelines>>(
            vkCreateGraphicsPipelines, AttachmentObserver::CreatePipeline);
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
        observeBegin.reset();
        observeView.reset();
        observePipeline.reset();
        vkDestroyDebugUtilsMessengerEXT(session->rhi.GetInstance(), messenger, nullptr);
        session.reset();
    }

    VulkanTexture* Texture(
        DataFormat format             = DataFormat::eR8G8B8A8UNORM,
        uint32_t mips                 = 3,
        uint32_t layers               = 4,
        RHITextureType type           = RHITextureType::e2D,
        SampleCount samples           = SampleCount::e1,
        uint32_t width                = 8,
        RHITextureUsageFlagBits usage = RHITextureUsageFlagBits::eColorAttachment)
    {
        RHITextureCreateInfo info{};
        info.type        = type;
        info.format      = format;
        info.width       = width;
        info.height      = 8;
        info.mipmaps     = mips;
        info.arrayLayers = layers;
        info.samples     = samples;
        info.usageFlags.SetFlags(usage, RHITextureUsageFlagBits::eTransferSrc,
                                 RHITextureUsageFlagBits::eTransferDst);
        auto* texture = static_cast<VulkanTexture*>(session->rhi.CreateTexture(info));
        textures.push_back(texture);
        return texture;
    }

    RHITextureView* View(VulkanTexture* texture,
                         uint32_t mip,
                         uint32_t layer,
                         uint32_t count                            = 1,
                         BitField<RHITextureAspectFlagBits> aspect = {})
    {
        RHITextureViewCreateInfo info{};
        info.type           = RHITextureType::e2D;
        info.format         = texture->GetFormat();
        info.baseMipLevel   = mip;
        info.baseArrayLayer = layer;
        info.arrayLayers    = count;
        info.aspect         = aspect;
        return texture->CreateView(info);
    }

    VulkanBuffer* Buffer()
    {
        RHIBufferCreateInfo info{};
        info.size         = 8192;
        info.allocateType = RHIBufferAllocateType::eCPURead;
        info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);
        auto* buffer = static_cast<VulkanBuffer*>(session->rhi.CreateBuffer(info));
        buffers.push_back(buffer);
        return buffer;
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

    void Initialize(VulkanTexture* texture, bool depthStencil = false)
    {
        Transition(texture, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const auto range = texture->GetVkSubresourceRange();
        if (depthStencil)
        {
            const VkClearDepthStencilValue clear{0.25f, 3};
            vkCmdClearDepthStencilImage(Commands(), texture->GetVkImage(),
                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
        }
        else
        {
            const VkClearColorValue clear{};
            vkCmdClearColorImage(Commands(), texture->GetVkImage(),
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
        }
        Transition(texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   depthStencil ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL :
                                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }

    void CopyAll(VulkanTexture* texture,
                 VulkanBuffer* buffer,
                 VkImageAspectFlags aspect,
                 uint32_t pixelBytes,
                 uint32_t baseOffset = 0)
    {
        HeapVector<VkBufferImageCopy> copies;
        uint32_t offset = baseOffset;
        for (uint32_t mip = 0; mip < texture->GetNumMipmaps(); ++mip)
        {
            VkBufferImageCopy copy{};
            copy.bufferOffset     = offset;
            copy.imageSubresource = {aspect, mip, 0, texture->GetArrayLayers()};
            copy.imageExtent      = {texture->GetWidth() >> mip, texture->GetHeight() >> mip, 1};
            copies.push_back(copy);
            offset += copy.imageExtent.width * copy.imageExtent.height * texture->GetArrayLayers() *
                pixelBytes;
        }
        ASSERT_LE(offset, 8192u);
        vkCmdCopyImageToBuffer(Commands(), texture->GetVkImage(),
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer->GetVkBuffer(),
                               static_cast<uint32_t>(copies.size()), copies.data());
    }

    void SubmitAndWait()
    {
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(Commands(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &barrier, 0, nullptr, 0, nullptr);
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

    RHIShader* Shader(bool depthOnly = false)
    {
        RHIShaderCreateInfo info{};
        for (const auto stage : {RHIShaderStage::eVertex, RHIShaderStage::eFragment})
        {
            info.stageFlags.SetFlag(RHIShaderStageToFlagBits(stage));
            info.spirvFileName[ToUnderlying(stage)] =
                std::filesystem::relative(
                    std::filesystem::path(RDG_REFLECTION_TEST_PATH) /
                        (stage == RHIShaderStage::eVertex ?
                             "pipeline.vert.spv" :
                             (depthOnly ? "pipeline_depth.frag.spv" : "pipeline.frag.spv")),
                    SPV_SHADER_PATH)
                    .generic_string();
        }
        auto* shader = session->rhi.CreateShader(info);
        shaders.push_back(shader);
        return shader;
    }

    RHIGfxPipelineCreateInfo PipelineInfo(RHIShader* shader,
                                          const RHIRenderingLayout& layout,
                                          SampleCount samples = SampleCount::e1)
    {
        RHIGfxPipelineCreateInfo info{};
        info.pShader                             = shader;
        info.pRenderingLayout                    = &layout;
        info.states.multiSampleState.sampleCount = samples;
        for (uint32_t i = 0; i < layout.numColorRenderTargets; ++i)
        {
            info.states.colorBlendState.AddAttachment();
        }
        return info;
    }

    void CheckColor(VulkanTexture* texture,
                    VulkanBuffer* buffer,
                    uint32_t selectedMip,
                    uint32_t firstLayer,
                    uint32_t layerCount,
                    bool partial,
                    bool draw)
    {
        const auto* pixels = buffer->Map();
        uint32_t offset    = 0;
        for (uint32_t mip = 0; mip < texture->GetNumMipmaps(); ++mip)
        {
            const uint32_t width  = texture->GetWidth() >> mip;
            const uint32_t height = texture->GetHeight() >> mip;
            for (uint32_t layer = 0; layer < texture->GetArrayLayers(); ++layer)
            {
                SCOPED_TRACE(testing::Message() << "mip " << mip << " layer " << layer);
                for (uint32_t y = 0; y < height; ++y)
                {
                    for (uint32_t x = 0; x < width; ++x, offset += 4)
                    {
                        const bool selected = mip == selectedMip && layer >= firstLayer &&
                            layer < firstLayer + layerCount &&
                            (!partial || (x >= 1 && x < 3 && y >= 1 && y < 3));
                        EXPECT_EQ(pixels[offset], selected ? 255 : 0);
                        EXPECT_EQ(pixels[offset + 1], 0);
                        EXPECT_EQ(pixels[offset + 2], 0);
                        EXPECT_EQ(pixels[offset + 3], selected ? (draw ? 64 : 255) : 0);
                    }
                }
            }
        }
        buffer->Unmap();
    }
};

class VulkanColorAttachmentTest :
    public VulkanAttachmentIntegrationTest,
    public testing::WithParamInterface<bool>
{};

TEST_P(VulkanColorAttachmentTest, ClearsSelectedMipLayersAndAreaOnly)
{
    const bool cube = GetParam();
    auto* texture   = Texture(DataFormat::eR8G8B8A8UNORM, 3, cube ? 6 : 4,
                              cube ? RHITextureType::eCube : RHITextureType::e2D);
    auto* view      = View(texture, 1, 1, 2);
    RHIRenderingLayout layout{};
    layout.SetRenderArea(1, 1, 2, 2);
    layout.AddColorRenderTarget(view, RHIRenderTargetLoadOp::eClear, RHIRenderTargetStoreOp::eStore,
                                RHIRenderTargetClearValue(Color(1, 0, 0, 1)));
    EXPECT_EQ(layout.numLayers, 2u);
    Initialize(texture);
    context->RHIBeginRendering(&layout);
    EXPECT_EQ(AttachmentObserver::colorView,
              static_cast<VulkanTextureView*>(view)->GetVkImageView());
    EXPECT_EQ(AttachmentObserver::layers, 2u);
    context->RHIEndRendering();
    Transition(texture, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    auto* buffer = Buffer();
    CopyAll(texture, buffer, VK_IMAGE_ASPECT_COLOR_BIT, 4);
    SubmitAndWait();
    CheckColor(texture, buffer, 1, 1, 2, true, false);
}

INSTANTIATE_TEST_SUITE_P(ImageTypes, VulkanColorAttachmentTest, testing::Bool());

TEST_F(VulkanAttachmentIntegrationTest, DrawsIntoOneMipAndNonzeroArrayLayer)
{
    auto* texture = Texture();
    RHIRenderingLayout layout{};
    layout.SetRenderArea(0, 0, 2, 2);
    layout.AddColorRenderTarget(View(texture, 2, 2), RHIRenderTargetLoadOp::eClear,
                                RHIRenderTargetStoreOp::eStore);
    auto* pipeline = session->rhi.CreatePipeline(PipelineInfo(Shader(), layout));
    pipelines.push_back(pipeline);
    Initialize(texture);
    context->RHIBeginRendering(&layout);
    context->RHIBindPipeline(pipeline);
    context->RHIDraw(3, 1, 0, 0);
    context->RHIEndRendering();
    Transition(texture, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    auto* buffer = Buffer();
    CopyAll(texture, buffer, VK_IMAGE_ASPECT_COLOR_BIT, 4);
    SubmitAndWait();
    CheckColor(texture, buffer, 2, 2, 1, false, true);
}

TEST_F(VulkanAttachmentIntegrationTest, TextureConvenienceReusesSingleMipViewAndRendersAllLayers)
{
    auto* texture = Texture();
    RHIRenderingLayout layout{};
    layout.SetRenderArea(0, 0, 8, 8);
    layout.AddColorRenderTarget(texture->GetFormat(), texture, RHIRenderTargetLoadOp::eClear,
                                RHIRenderTargetStoreOp::eStore,
                                RHIRenderTargetClearValue(Color(1, 0, 0, 1)));
    EXPECT_EQ(layout.numLayers, 4u);
    const uint32_t viewCount = AttachmentObserver::viewCreates;
    Initialize(texture);
    context->RHIBeginRendering(&layout);
    const VkImageView firstView = AttachmentObserver::colorView;
    EXPECT_NE(firstView, texture->GetVkImageView());
    EXPECT_EQ(AttachmentObserver::viewCreates, viewCount + 1);
    context->RHIEndRendering();
    Transition(texture, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    context->RHIBeginRendering(&layout);
    EXPECT_EQ(AttachmentObserver::colorView, firstView);
    EXPECT_EQ(AttachmentObserver::viewCreates, viewCount + 1);
    context->RHIEndRendering();
    EXPECT_EQ(texture->GetDefaultView()->GetSubResourceRange().levelCount, 3u);
    Transition(texture, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    auto* buffer = Buffer();
    CopyAll(texture, buffer, VK_IMAGE_ASPECT_COLOR_BIT, 4);
    SubmitAndWait();
    CheckColor(texture, buffer, 0, 0, 4, false, false);
}

class VulkanDepthAttachmentTest :
    public VulkanAttachmentIntegrationTest,
    public testing::WithParamInterface<std::pair<DataFormat, int64_t>>
{};

TEST_P(VulkanDepthAttachmentTest, ClearsOnlySelectedDepthStencilMipAndLayers)
{
    const auto [format, aspectMask] = GetParam();
    const bool depth                = format != DataFormat::eS8UInt;
    const bool stencil              = format != DataFormat::eD32SFloat;
    const BitField<RHITextureAspectFlagBits> aspect(aspectMask);
    const bool renderDepth =
        depth && (aspect.IsEmpty() || aspect.HasFlag(RHITextureAspectFlagBits::eDepth));
    const bool renderStencil =
        stencil && (aspect.IsEmpty() || aspect.HasFlag(RHITextureAspectFlagBits::eStencil));
    auto* texture = Texture(format, 3, 4, RHITextureType::e2D, SampleCount::e1, 8,
                            RHITextureUsageFlagBits::eDepthStencilAttachment);
    RHIRenderingLayout layout{};
    layout.SetRenderArea(0, 0, 4, 4);
    layout.AddDepthStencilRenderTarget(
        View(texture, 1, 1, 2, aspect), RHIRenderTargetLoadOp::eClear,
        RHIRenderTargetStoreOp::eStore, RHIRenderTargetClearValue(0.75f, 9));
    EXPECT_EQ(layout.numLayers, 2u);
    pipelines.push_back(session->rhi.CreatePipeline(PipelineInfo(Shader(true), layout)));
    const auto vkFormat = texture->GetVkImageCreateInfo().format;
    EXPECT_EQ(AttachmentObserver::depthFormat, renderDepth ? vkFormat : VK_FORMAT_UNDEFINED);
    EXPECT_EQ(AttachmentObserver::stencilFormat, renderStencil ? vkFormat : VK_FORMAT_UNDEFINED);
    Initialize(texture, true);
    context->RHIBeginRendering(&layout);
    context->RHIEndRendering();
    Transition(texture, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    auto* buffer = Buffer();
    if (depth)
    {
        CopyAll(texture, buffer, VK_IMAGE_ASPECT_DEPTH_BIT, 4);
    }
    if (stencil)
    {
        CopyAll(texture, buffer, VK_IMAGE_ASPECT_STENCIL_BIT, 1, 4096);
    }
    SubmitAndWait();
    const auto* bytes  = buffer->Map();
    const auto* depths = reinterpret_cast<const float*>(bytes);
    uint32_t offset    = 0;
    for (uint32_t mip = 0; mip < 3; ++mip)
    {
        for (uint32_t layer = 0; layer < 4; ++layer)
        {
            SCOPED_TRACE(testing::Message() << "mip " << mip << " layer " << layer);
            const bool selected = mip == 1 && (layer == 1 || layer == 2);
            for (uint32_t i = 0; i < (8u >> mip) * (8u >> mip); ++i, ++offset)
            {
                if (depth)
                {
                    EXPECT_FLOAT_EQ(depths[offset], selected && renderDepth ? 0.75f : 0.25f);
                }
                if (stencil)
                {
                    EXPECT_EQ(bytes[4096 + offset], selected && renderStencil ? 9 : 3);
                }
            }
        }
    }
    buffer->Unmap();
}

INSTANTIATE_TEST_SUITE_P(Aspects,
                         VulkanDepthAttachmentTest,
                         testing::Values(std::pair{DataFormat::eD32SFloat, int64_t(0)},
                                         std::pair{DataFormat::eS8UInt, int64_t(0)},
                                         std::pair{DataFormat::eD32SFloatS8UInt, int64_t(0)},
                                         std::pair{DataFormat::eD32SFloatS8UInt,
                                                   int64_t(RHITextureAspectFlagBits::eDepth)},
                                         std::pair{DataFormat::eD32SFloatS8UInt,
                                                   int64_t(RHITextureAspectFlagBits::eStencil)},
                                         std::pair{
                                             DataFormat::eD32SFloatS8UInt,
                                             int64_t(RHITextureAspectFlagBits::eDepth) |
                                                 int64_t(RHITextureAspectFlagBits::eStencil)}));

TEST_F(VulkanAttachmentIntegrationTest, DifferentExtentsAndLayerCountsAcceptTheirCommonRenderArea)
{
    auto* first =
        Texture(DataFormat::eR8G8B8A8UNORM, 1, 4, RHITextureType::e2D, SampleCount::e1, 16);
    auto* second = Texture(DataFormat::eR8G8B8A8UNORM, 1, 2);
    RHIRenderingLayout layout{};
    layout.SetRenderArea(0, 0, 8, 8);
    layout.AddColorRenderTarget(first->GetFormat(), first, RHIRenderTargetLoadOp::eClear,
                                RHIRenderTargetStoreOp::eStore);
    layout.AddColorRenderTarget(second->GetFormat(), second, RHIRenderTargetLoadOp::eClear,
                                RHIRenderTargetStoreOp::eStore);
    EXPECT_EQ(layout.numLayers, 2u);
    Initialize(first);
    Initialize(second);
    context->RHIBeginRendering(&layout);
    context->RHIEndRendering();
    SubmitAndWait();
}

TEST_F(VulkanAttachmentIntegrationTest,
       MultisampleColorAndDepthInferActualSamplesAndCreateMatchingPipeline)
{
    auto* color = Texture(DataFormat::eR8G8B8A8UNORM, 1, 1, RHITextureType::e2D, SampleCount::e4);
    auto* depth = Texture(DataFormat::eD32SFloat, 1, 1, RHITextureType::e2D, SampleCount::e4, 8,
                          RHITextureUsageFlagBits::eDepthStencilAttachment);
    RHIRenderingLayout layout{};
    layout.SetRenderArea(0, 0, 8, 8);
    layout.AddColorRenderTarget(color->GetFormat(), color, RHIRenderTargetLoadOp::eClear,
                                RHIRenderTargetStoreOp::eStore);
    layout.AddDepthStencilRenderTarget(depth->GetFormat(), depth, RHIRenderTargetLoadOp::eClear,
                                       RHIRenderTargetStoreOp::eStore);
    EXPECT_EQ(layout.colorRenderTargets[0].numSamples, SampleCount::e4);
    EXPECT_EQ(layout.depthStencilRenderTarget.numSamples, SampleCount::e4);
    auto info = PipelineInfo(Shader(), layout, SampleCount::e1);
    EXPECT_THROW(session->rhi.CreatePipeline(info), std::runtime_error);
    info.states.multiSampleState.sampleCount = SampleCount::e4;
    pipelines.push_back(session->rhi.CreatePipeline(info));
    Transition(color, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    Transition(depth, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    context->RHIBeginRendering(&layout);
    context->RHIBindPipeline(pipelines.back());
    context->RHIDraw(3, 1, 0, 0);
    context->RHIEndRendering();
    SubmitAndWait();
}

class VulkanInvalidViewTest :
    public VulkanAttachmentIntegrationTest,
    public testing::WithParamInterface<int>
{};

TEST_P(VulkanInvalidViewTest, RejectsInvalidViewBeforeCallingVulkan)
{
    auto* texture = Texture();
    RHITextureViewCreateInfo info{};
    info.type   = RHITextureType::e2D;
    info.format = texture->GetFormat();
    switch (GetParam())
    {
        case 0: info.mipLevels = 0; break;
        case 1: info.baseMipLevel = 3; break;
        case 2:
            info.baseMipLevel = 1;
            info.mipLevels    = UINT32_MAX;
            break;
        case 3: info.arrayLayers = 0; break;
        case 4: info.baseArrayLayer = 4; break;
        case 5:
            info.baseArrayLayer = 1;
            info.arrayLayers    = UINT32_MAX;
            break;
        case 6: info.type = RHITextureType::e3D; break;
        case 7: info.format = DataFormat::eR8G8B8A8SRGB; break;
        case 8: info.aspect.SetFlag(RHITextureAspectFlagBits::eDepth); break;
    }
    const auto count = AttachmentObserver::viewCreates;
    EXPECT_THROW(texture->CreateView(info), std::runtime_error);
    EXPECT_EQ(AttachmentObserver::viewCreates, count);
}

INSTANTIATE_TEST_SUITE_P(RangesAndTypes, VulkanInvalidViewTest, testing::Range(0, 9));

class VulkanInvalidAttachmentTest :
    public VulkanAttachmentIntegrationTest,
    public testing::WithParamInterface<int>
{};

TEST_P(VulkanInvalidAttachmentTest, RejectsInvalidLayoutBeforeRecordingRendering)
{
    auto* texture = Texture();
    RHIRenderingLayout layout{};
    layout.SetRenderArea(0, 0, 4, 4);
    layout.AddColorRenderTarget(View(texture, 1, 1, 2), RHIRenderTargetLoadOp::eClear,
                                RHIRenderTargetStoreOp::eStore);
    switch (GetParam())
    {
        case 0: layout.SetRenderArea(1, 0, 4, 4); break;
        case 1: layout.SetRenderArea(0, 0, 4, 5); break;
        case 2: layout.numLayers = 3; break;
        case 3: layout.numLayers = 0; break;
        case 4: layout.SetRenderArea(-1, 0, 4, 4); break;
        case 5: layout.SetRenderArea(0, 0, 0, 4); break;
        case 6: layout.colorRenderTargets[0].pTextureView = texture->GetDefaultView(); break;
        case 7: layout.colorRenderTargets[0].format = DataFormat::eR8G8B8A8SRGB; break;
        case 8: layout.colorRenderTargets[0].pTexture = Texture(); break;
        case 9: layout.colorRenderTargets[0].numSamples = SampleCount::e4; break;
        case 10: layout.numColorRenderTargets = MAX_NUM_COLOR_ATTACHMENTS + 1; break;
        case 11: layout.colorRenderTargets[0].pTexture = nullptr; break;
        case 12:
            layout.AddColorRenderTarget(
                View(Texture(DataFormat::eD32SFloat, 1, 2, RHITextureType::e2D, SampleCount::e1, 8,
                             RHITextureUsageFlagBits::eDepthStencilAttachment),
                     0, 0, 2),
                RHIRenderTargetLoadOp::eClear, RHIRenderTargetStoreOp::eStore);
            break;
        case 13:
            layout.AddDepthStencilRenderTarget(View(texture, 1, 1, 2),
                                               RHIRenderTargetLoadOp::eClear,
                                               RHIRenderTargetStoreOp::eStore);
            break;
        case 14:
            layout.AddColorRenderTarget(
                View(Texture(DataFormat::eR8G8B8A8UNORM, 1, 2, RHITextureType::e2D, SampleCount::e1,
                             8, RHITextureUsageFlagBits::eSampled),
                     0, 0, 2),
                RHIRenderTargetLoadOp::eClear, RHIRenderTargetStoreOp::eStore);
            break;
        case 15:
            layout.AddColorRenderTarget(View(Texture(DataFormat::eR8G8B8A8UNORM, 1, 2,
                                                     RHITextureType::e2D, SampleCount::e4),
                                             0, 0, 2),
                                        RHIRenderTargetLoadOp::eClear,
                                        RHIRenderTargetStoreOp::eStore);
            break;
    }
    EXPECT_THROW(context->RHIBeginRendering(&layout), std::runtime_error);
    EXPECT_EQ(AttachmentObserver::begins, 0u);
}

INSTANTIATE_TEST_SUITE_P(Compatibility, VulkanInvalidAttachmentTest, testing::Range(0, 16));

TEST_F(VulkanAttachmentIntegrationTest, FailedViewCreationDoesNotPublishViewAndCanBeRetried)
{
    auto* texture                = Texture();
    AttachmentObserver::failView = true;
    EXPECT_THROW(View(texture, 1, 1), std::runtime_error);
    // Also exercise cleanup when the texture's initial default view cannot be created.
    EXPECT_THROW(Texture(), std::runtime_error);
    AttachmentObserver::failView = false;
    EXPECT_NE(View(texture, 1, 1), nullptr);
    EXPECT_NE(Texture(), nullptr);
}

TEST_F(VulkanAttachmentIntegrationTest, LayoutResetAndAttachmentCapacityPreserveInvariants)
{
    auto* texture = Texture();
    RHIRenderingLayout layout{};
    for (uint32_t i = 0; i < MAX_NUM_COLOR_ATTACHMENTS; ++i)
    {
        layout.AddColorRenderTarget(View(texture, 1, 1, 2), RHIRenderTargetLoadOp::eClear,
                                    RHIRenderTargetStoreOp::eStore);
    }
    EXPECT_THROW(layout.AddColorRenderTarget(texture->GetDefaultView(),
                                             RHIRenderTargetLoadOp::eClear,
                                             RHIRenderTargetStoreOp::eStore),
                 std::runtime_error);
    EXPECT_EQ(layout.numColorRenderTargets, MAX_NUM_COLOR_ATTACHMENTS);
    layout.Reset();
    EXPECT_EQ(layout.GetTotalNumRenderTargets(), 0u);
    EXPECT_EQ(layout.numLayers, 1u);
    EXPECT_EQ(layout.colorRenderTargets[0].pTextureView, nullptr);
    EXPECT_THROW(layout.SetRenderArea(1, 0, UINT32_MAX, 1), std::runtime_error);
}
} // namespace
