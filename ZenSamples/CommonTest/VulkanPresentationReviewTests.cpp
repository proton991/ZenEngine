#include "VulkanIntegrationFixture.h"
#include "Graphics/RHI/RHIOptions.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RenderCore/V2/RenderGraph/RenderGraph.h"
#include "Graphics/VulkanRHI/VulkanCommandList.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanTexture.h"
#include "Graphics/VulkanRHI/VulkanSynchronization.h"
#include "Platform/NativeWindow.h"
#include <gtest/gtest.h>
#include <tuple>

namespace
{
using namespace zen;
using namespace zen::rc;

// Parameters: RHI worker, async compute, and ray tracing disabled before device creation.
class VulkanPresentationReviewTest : public testing::TestWithParam<std::tuple<bool, bool, bool>>
{
protected:
    void SetUp() override
    {
        previousRayTracingDisabled = RHIOptions::GetInstance().RayTracingDisabled();
        RHIOptions::GetInstance().SetRayTracingDisabled(std::get<2>(GetParam()));
        platform::WindowConfig config;
        config.visible = false;
        config.width = config.height = 256;
        window                       = MakeUnique<platform::NativeWindow>(config);
        device   = MakeUnique<RenderDevice>(RHIAPIType::eVulkan, 2,
                                            std::get<0>(GetParam()) ? RHIExecutionMode::eThreaded : RHIExecutionMode::eInline,
                                            std::get<1>(GetParam()) ? AsyncComputeMode::eAuto : AsyncComputeMode::eDisabled);
        viewport = device->CreateViewport(window.Get(), 256, 256, false);
        ASSERT_NE(viewport, nullptr);
        device->Init(viewport);
    }

    void TearDown() override
    {
        if (device)
        {
            ShaderProgramManager::GetInstance().Destroy();
            device->Destroy();
        }
        RHIOptions::GetInstance().SetRayTracingDisabled(previousRayTracingDisabled);
    }

public:
    void ReadBack(RHIBuffer* destination)
    {
        VulkanTexture*            source = static_cast<VulkanTexture*>(viewport->GetColorBackBuffer());
        FVulkanCommandListContext context(RHICommandContextType::eGraphics, GVulkanRHI->GetDevice());
        ASSERT_TRUE(context.EnsureRecording());
        const VkCommandBuffer commands = context.GetCommandBuffer()->GetVkHandle();
        VulkanPipelineBarrier before;
        before.AddImageBarrier(source->GetVkImage(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, source->GetVkSubresourceRange(),
                               VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                               VK_ACCESS_TRANSFER_READ_BIT);
        before.Execute(commands, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        RHIBufferTextureCopyRegion region{};
        region.textureSubresources.aspect = BitField<RHITextureAspectFlagBits>(RHITextureAspectFlagBits::eColor);
        region.textureSize                = {source->GetWidth(), source->GetHeight(), 1};
        context.RHICopyTextureToBuffer(source, destination, MakeVecView(region));
        VulkanPipelineBarrier after;
        after.AddImageBarrier(source->GetVkImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, source->GetVkSubresourceRange(),
                              VK_ACCESS_TRANSFER_READ_BIT,
                              VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        after.AddMemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        after.Execute(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_HOST_BIT);
        ASSERT_EQ(context.SubmitRecordedWorkloads(), RHISubmissionResult::eSuccess);
        GVulkanRHI->WaitDeviceIdle();
    }

    UniquePtr<platform::NativeWindow> window;
    UniquePtr<RenderDevice>           device;
    RHIViewport*                      viewport{nullptr};
    bool                              previousRayTracingDisabled{false};
};

TEST_P(VulkanPresentationReviewTest, ClearOnlyGraphPresentsAndNextAttachmentFrameRemainsValid)
{
    RenderGraph& graph = *device->GetCurrentFrameRDG();
    ASSERT_TRUE(graph.Begin());
    graph.AddTransferPass("WorkspaceClear").ClearTexture(viewport->GetColorBackBuffer(), Color(1.0f));
    ASSERT_TRUE(graph.End());
    ASSERT_EQ(device->ExecuteRenderGraph(viewport), RenderFrameResult::eSubmitted);
    device->WaitForIdle();
    ASSERT_TRUE(device->PollFrameSubmissions(true));

    RHIBufferCreateInfo info{};
    info.size         = uint64_t(viewport->GetWidth()) * viewport->GetHeight() * 4;
    info.allocateType = RHIBufferAllocateType::eCPURead;
    info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);
    RHIBuffer* readback = device->CreateBuffer(info);
    ASSERT_NE(readback, nullptr);
    GetRHIThread().Invoke(&VulkanPresentationReviewTest::ReadBack, this, readback);
    const uint8_t* pixels = readback->Map();
    ASSERT_NE(pixels, nullptr);
    for (uint64_t i = 0; i < info.size; ++i)
    {
        ASSERT_EQ(pixels[i], 255u) << i;
    }
    readback->Unmap();
    device->DestroyBuffer(readback);

    RHICommandListExecutor* executor = static_cast<RHICommandListExecutor*>(GDynamicRHI);
    RHICommandListPtr       commands(RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));
    RHIRenderingLayout      layout{};
    layout.SetRenderArea(0, 0, viewport->GetWidth(), viewport->GetHeight());
    layout.AddColorRenderTarget(viewport->GetSwapchainFormat(), viewport->GetColorBackBuffer(), RHIRenderTargetLoadOp::eLoad,
                                RHIRenderTargetStoreOp::eStore);
    commands->BeginRendering(&layout);
    commands->EndRendering();
    const RHIBatchResult result = executor->SubmitFrame(*commands, viewport).Wait();
    EXPECT_EQ(result.submission, RHISubmissionResult::eSuccess);
    EXPECT_TRUE(result.presented);
    device->WaitForIdle();
}

TEST_P(VulkanPresentationReviewTest, RendererInitializationWarmsRegisteredComputeKeysForLaterReuse)
{
    ShaderProgramManager& programs = ShaderProgramManager::GetInstance();
    programs.BuildShaderPrograms(device.Get());
    device->InitializeRendererServer();
    const HeapVector<NameID>&  warmups = programs.GetInitializationWarmups();
    const PipelineCacheMetrics warmed  = device->GetPipelineCacheMetrics();
    EXPECT_EQ(warmups.size(), 3u);
    EXPECT_EQ(warmed.creations, warmups.size());
    for (NameID name : warmups)
    {
        SCOPED_TRACE(name.CStr());
        // Only programs this device registered, and therefore supports, are warmed.
        ShaderProgram* program = programs.RequestShaderProgram(name);
        ASSERT_NE(program, nullptr);
        ASSERT_NE(device->GetOrCreateComputePipeline(program->GetShader()), nullptr);
    }
    EXPECT_EQ(device->GetPipelineCacheMetrics().creations, warmed.creations);
    EXPECT_EQ(device->GetPipelineCacheMetrics().hits, warmed.hits + warmups.size());
}

TEST_P(VulkanPresentationReviewTest, RegisteredProgramsCreateShadersAndRequireRayQueryOnlyWhenSupported)
{
    ShaderProgramManager& programs = ShaderProgramManager::GetInstance();
    programs.BuildShaderPrograms(device.Get());
    const bool rayQuery = device->GetGPUInfo().rayQuery.IsUsable();
    EXPECT_FALSE(std::get<2>(GetParam()) && rayQuery);
    uint32_t hardwarePrograms = 0;
    for (const std::pair<const NameID, ShaderProgram*>& entry : programs.GetPrograms())
    {
        SCOPED_TRACE(entry.first.CStr());
        // Shader creation validates the merged stages, including cross-stage descriptor names.
        RHIShader* shader = entry.second->GetShader();
        ASSERT_NE(shader, nullptr);
        hardwarePrograms += shader->GetSRDCountByType(RHIShaderResourceType::eAccelerationStructure) != 0 ? 1 : 0;
    }
    // Ray-query programs exist exactly when the device can run them.
    EXPECT_EQ(hardwarePrograms != 0, rayQuery);
}

INSTANTIATE_TEST_SUITE_P(InlineThreadedAndAsync,
                         VulkanPresentationReviewTest,
                         testing::Combine(testing::Bool(), testing::Bool(), testing::Values(false)));

INSTANTIATE_TEST_SUITE_P(RayTracingDisabled,
                         VulkanPresentationReviewTest,
                         testing::Combine(testing::Values(false), testing::Values(false), testing::Values(true)));
} // namespace
