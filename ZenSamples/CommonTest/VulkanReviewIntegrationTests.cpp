#include "VulkanIntegrationFixture.h"
#include "ScopedVulkanCall.h"
#include "Graphics/VulkanRHI/VulkanBuffer.h"
#include "Graphics/VulkanRHI/VulkanCommandList.h"
#include "Graphics/VulkanRHI/VulkanDebug.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Platform/NativeWindow.h"
#include <gtest/gtest.h>

namespace
{
using namespace zen;

VKAPI_ATTR VkBool32 VKAPI_CALL ReviewValidation(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                                VkDebugUtilsMessageTypeFlagsEXT,
                                                const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                void*)
{
    ADD_FAILURE() << RHIErrorText(data->pMessage, "empty validation message");
    return VK_FALSE;
}

class VulkanReviewIntegrationTest : public testing::TestWithParam<bool>
{
protected:
    void SetUp() override
    {
        session = MakeUnique<test::VulkanSession>();
        if (GetParam())
        {
            session->rhi.GetDevice()->GetExtensionFlags().hasTimelineSemaphore = 0;
        }
        VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        info.pfnUserCallback = ReviewValidation;
        ASSERT_EQ(vkCreateDebugUtilsMessengerEXT(session->rhi.GetInstance(), &info, nullptr, &messenger), VK_SUCCESS);
    }

    void TearDown() override
    {
        session->rhi.WaitDeviceIdle();
        vkDestroyDebugUtilsMessengerEXT(session->rhi.GetInstance(), messenger, nullptr);
        session.Reset();
    }

    UniquePtr<test::VulkanSession> session;
    VkDebugUtilsMessengerEXT       messenger{VK_NULL_HANDLE};
};

TEST_P(VulkanReviewIntegrationTest, InvalidImageAndBufferDescriptionsNeverReachInvalidNativeUsage)
{
    RHITextureCreateInfo valid{};
    valid.type   = RHITextureType::e2D;
    valid.format = DataFormat::eR8G8B8A8UNORM;
    valid.width = valid.height = 8;
    valid.usageFlags.SetFlag(RHITextureUsageFlagBits::eSampled);
    for (uint32_t scenario = 0; scenario < 5; ++scenario)
    {
        RHITextureCreateInfo info = valid;
        if (scenario == 0)
        {
            info.format = DataFormat::eR8G8B8A8SRGB;
            info.usageFlags.SetFlag(RHITextureUsageFlagBits::eStorage);
        }
        if (scenario == 1)
        {
            info.type        = RHITextureType::eCube;
            info.arrayLayers = 6;
            info.width       = 4;
        }
        if (scenario == 2)
        {
            info.type = RHITextureType::e1D;
        }
        if (scenario == 3)
        {
            info.samples = SampleCount::e4;
            info.mipmaps = 2;
        }
        if (scenario == 4)
        {
            info.width = 0;
        }
        RHIResourcePtr<RHITexture> texture = RHIResourcePtr<RHITexture>::Adopt(session->rhi.CreateTexture(info));
        EXPECT_EQ(texture.Get(), nullptr) << scenario;
    }
    RHIResourcePtr<RHITexture> texture = RHIResourcePtr<RHITexture>::Adopt(session->rhi.CreateTexture(valid));
    ASSERT_NE(texture.Get(), nullptr);
    for (uint32_t scenario = 0; scenario < 3; ++scenario)
    {
        RHIBufferCreateInfo info{};
        info.size         = scenario == 1 ? 5 : 16;
        info.allocateType = scenario == 2 ? RHIBufferAllocateType::eNone : RHIBufferAllocateType::eGPU;
        info.usageFlags.SetFlag(scenario == 1 ? RHIBufferUsageFlagBits::eTextureBuffer
                                              : RHIBufferUsageFlagBits::eStorageBuffer);
        RHIResourcePtr<RHIBuffer> buffer = RHIResourcePtr<RHIBuffer>::Adopt(session->rhi.CreateBuffer(info));
        if (scenario == 2)
        {
            EXPECT_EQ(buffer.Get(), nullptr);
        }
        else
        {
            ASSERT_NE(buffer.Get(), nullptr);
            EXPECT_FALSE(buffer->SetTexelFormat(DataFormat::eR32UInt));
        }
    }
}

TEST_P(VulkanReviewIntegrationTest, InvalidSamplersAreRejectedAndOptionalCapabilitiesMatchDevice)
{
    RHISamplerCreateInfo info{};
    info.unnormalizedUVW = true;
    EXPECT_EQ(session->rhi.CreateSampler(info), nullptr);
    info.maxLod                        = 0;
    RHIResourcePtr<RHISampler> sampler = RHIResourcePtr<RHISampler>::Adopt(session->rhi.CreateSampler(info));
    ASSERT_NE(sampler.Get(), nullptr);
    const RHIGPUInfo& gpu = session->rhi.QueryGPUInfo();
    EXPECT_EQ(gpu.supportImageCubeArray, session->rhi.GetDevice()->GetPhysicalDeviceFeatures().imageCubeArray != VK_FALSE);
    EXPECT_EQ(gpu.supportSamplerMirrorClampToEdge,
              session->rhi.GetDevice()->GetExtensionFlags().hasSamplerMirrorClampToEdge != 0);
    EXPECT_EQ(gpu.maxDrawIndexedIndexValue,
              session->rhi.GetDevice()->GetPhysicalDeviceProperties().limits.maxDrawIndexedIndexValue);
    RHITextureCreateInfo cube{};
    cube.type  = RHITextureType::eCube;
    cube.width = cube.height = 8;
    cube.arrayLayers         = 12;
    cube.format              = DataFormat::eR8G8B8A8UNORM;
    cube.usageFlags.SetFlag(RHITextureUsageFlagBits::eSampled);
    RHIResourcePtr<RHITexture> texture = RHIResourcePtr<RHITexture>::Adopt(session->rhi.CreateTexture(cube));
    EXPECT_EQ(texture.Get() != nullptr, gpu.supportImageCubeArray);

    // Exercise the published-feature rejection even on a cube-array-capable device.
    RHIGPUInfo& reported                = const_cast<RHIGPUInfo&>(session->rhi.QueryGPUInfo());
    const bool  cubeArrays              = reported.supportImageCubeArray;
    reported.supportImageCubeArray      = false;
    RHIResourcePtr<RHITexture> rejected = RHIResourcePtr<RHITexture>::Adopt(session->rhi.CreateTexture(cube));
    reported.supportImageCubeArray      = cubeArrays;
    EXPECT_EQ(rejected.Get(), nullptr);
}

TEST_P(VulkanReviewIntegrationTest, InvalidShaderLayoutsReturnNull)
{
    for (const char* file : {"review_runtime_array.comp.spv", "review_reserved_set.comp.spv"})
    {
        RHIShaderCreateInfo info{};
        test::SetShaderStage(info, RHIShaderStage::eCompute, file);
        RHIResourcePtr<RHIShader> shader = RHIResourcePtr<RHIShader>::Adopt(session->rhi.CreateShader(info));
        EXPECT_EQ(shader.Get(), nullptr);
    }
    RHIShaderCreateInfo duplicate{};
    test::SetShaderStage(duplicate, RHIShaderStage::eVertex, "review_duplicate.vert.spv");
    test::SetShaderStage(duplicate, RHIShaderStage::eFragment, "review_duplicate.frag.spv");
    RHIResourcePtr<RHIShader> shader = RHIResourcePtr<RHIShader>::Adopt(session->rhi.CreateShader(duplicate));
    EXPECT_EQ(shader.Get(), nullptr);
}

struct ImmediateSubmitFailure
{
    static inline VkResult                result{VK_ERROR_OUT_OF_DEVICE_MEMORY};
    static VKAPI_ATTR VkResult VKAPI_CALL Submit(VkQueue, uint32_t, const VkSubmitInfo*, VkFence)
    {
        return result;
    }
};

TEST_P(VulkanReviewIntegrationTest, ViewportRejectionAllowsRetryAndResizePreservesTheCause)
{
    platform::WindowConfig config;
    config.visible = false;
    config.width = config.height = 96;
    platform::NativeWindow window(config);
    {
        test::ScopedVulkanCall<PFN_vkQueueSubmit> failure(vkQueueSubmit, ImmediateSubmitFailure::Submit);
        EXPECT_EQ(session->rhi.CreateViewport(&window, 96, 96, false), nullptr);
        EXPECT_FALSE(session->rhi.AreSubmissionsBlocked());
        EXPECT_EQ(session->rhi.GetLastSubmissionError().code, RHIErrorCode::eOutOfDeviceMemory);
    }
    RHIResourcePtr<RHIViewport> viewport =
        RHIResourcePtr<RHIViewport>::Adopt(session->rhi.CreateViewport(&window, 96, 96, false));
    ASSERT_NE(viewport.Get(), nullptr);
    {
        test::ScopedVulkanCall<PFN_vkQueueSubmit> failure(vkQueueSubmit, ImmediateSubmitFailure::Submit);
        viewport->Resize(112, 112);
    }
    EXPECT_TRUE(session->rhi.AreSubmissionsBlocked());
    EXPECT_EQ(session->rhi.GetTerminalError().code, RHIErrorCode::eOutOfDeviceMemory);
    EXPECT_STREQ(session->rhi.GetTerminalError().operation, "vkQueueSubmit");
}

TEST_P(VulkanReviewIntegrationTest, ViewportDeviceLossBlocksWithNativeCause)
{
    platform::WindowConfig config;
    config.visible = false;
    config.width = config.height = 96;
    platform::NativeWindow window(config);
    ImmediateSubmitFailure::result = VK_ERROR_DEVICE_LOST;
    {
        test::ScopedVulkanCall<PFN_vkQueueSubmit> failure(vkQueueSubmit, ImmediateSubmitFailure::Submit);
        EXPECT_EQ(session->rhi.CreateViewport(&window, 96, 96, false), nullptr);
    }
    ImmediateSubmitFailure::result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    EXPECT_TRUE(session->rhi.AreSubmissionsBlocked());
    EXPECT_EQ(session->rhi.GetTerminalError().code, RHIErrorCode::eDeviceLost);
}

struct ImageViewFailure
{
    static VKAPI_ATTR VkResult VKAPI_CALL Create(VkDevice,
                                                 const VkImageViewCreateInfo*,
                                                 const VkAllocationCallbacks*,
                                                 VkImageView* pView)
    {
        *pView = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
};

TEST_P(VulkanReviewIntegrationTest, CreationFailuresStayOutOfSubmissionErrorsAndReachViewportCause)
{
    RHITextureCreateInfo info{};
    info.type   = RHITextureType::e2D;
    info.format = DataFormat::eR8G8B8A8UNORM;
    info.width = info.height = 8;
    info.usageFlags.SetFlag(RHITextureUsageFlagBits::eSampled);
    {
        test::ScopedVulkanCall<PFN_vkCreateImageView> failure(vkCreateImageView, ImageViewFailure::Create);
        EXPECT_EQ(session->rhi.CreateTexture(info), nullptr);
    }
    EXPECT_FALSE(session->rhi.AreSubmissionsBlocked());
    EXPECT_FALSE(session->rhi.GetLastSubmissionError().IsFailure());
    EXPECT_EQ(session->rhi.GetLastCreationError().code, RHIErrorCode::eOutOfDeviceMemory);
    EXPECT_STREQ(session->rhi.GetLastCreationError().operation, "vkCreateImageView");

    platform::WindowConfig config;
    config.visible = false;
    config.width = config.height = 96;
    platform::NativeWindow      window(config);
    RHIResourcePtr<RHIViewport> viewport =
        RHIResourcePtr<RHIViewport>::Adopt(session->rhi.CreateViewport(&window, 96, 96, false));
    ASSERT_NE(viewport.Get(), nullptr);
    {
        test::ScopedVulkanCall<PFN_vkCreateImageView> failure(vkCreateImageView, ImageViewFailure::Create);
        viewport->Resize(112, 112);
    }
    EXPECT_TRUE(session->rhi.AreSubmissionsBlocked());
    EXPECT_EQ(session->rhi.GetTerminalError().code, RHIErrorCode::eOutOfDeviceMemory);
    EXPECT_STREQ(session->rhi.GetTerminalError().operation, "vkCreateImageView");
}

TEST_P(VulkanReviewIntegrationTest, DebugNamesWithoutExtensionDoNotCallNativeFunction)
{
    InstanceExtensionFlags& flags   = session->rhi.GetInstanceExtensionFlags();
    const bool              enabled = flags.hasDebugUtils != 0;
    flags.hasDebugUtils             = 0;
    test::ScopedVulkanCall<PFN_vkSetDebugUtilsObjectNameEXT> unavailable(vkSetDebugUtilsObjectNameEXT, nullptr);
    session->rhi.GetDevice()->SetObjectName(VK_OBJECT_TYPE_IMAGE, 0, "no_extension");
    VulkanDebug debug;
    debug.SetTextureDebugName(nullptr, "null_texture");
    debug.SetPipelineDebugName(nullptr, "null_pipeline");
    flags.hasDebugUtils = enabled;
}

INSTANTIATE_TEST_SUITE_P(TimelineAndFences, VulkanReviewIntegrationTest, testing::Bool());
} // namespace
