#include "Graphics/RHI/RHIOptions.h"
#include "Graphics/VulkanRHI/VulkanTexture.h"
#include "Graphics/VulkanRHI/VulkanTypes.h"
#include "Graphics/VulkanRHI/VulkanPipeline.h"
#include <gtest/gtest.h>
#include <limits>
#include <type_traits>

using namespace zen;

namespace zen
{
VKAPI_ATTR VkBool32 VKAPI_CALL DebugUtilsMessengerCallback(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                                           VkDebugUtilsMessageTypeFlagsEXT,
                                                           const VkDebugUtilsMessengerCallbackDataEXT*,
                                                           void*);
}

TEST(VulkanReviewTest, ValidationDefaultsFollowBuildAndAllowExplicitOverrides)
{
    RHIOptions& options  = RHIOptions::GetInstance();
    const bool  original = options.ValidationEnabled();
#if defined(NDEBUG)
    EXPECT_FALSE(original);
#else
    EXPECT_TRUE(original);
#endif
    options.SetValidationEnabled(false);
    EXPECT_FALSE(options.ValidationEnabled());
    options.SetValidationEnabled(true);
    EXPECT_TRUE(options.ValidationEnabled());
    options.SetValidationEnabled(original);
}

TEST(VulkanReviewTest, NullDebugMessageFieldsAreSafe)
{
    VkDebugUtilsMessengerCallbackDataEXT data{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT};
    EXPECT_EQ(DebugUtilsMessengerCallback(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
                                          VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, &data, nullptr),
              VK_FALSE);
    EXPECT_EQ(DebugUtilsMessengerCallback(VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                                          VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, &data, nullptr),
              VK_FALSE);
}

namespace
{
class ReviewCountedResource final : public RHIResource
{
public:
    ReviewCountedResource() : RHIResource(RHIResourceType::eSampler) {}

protected:
    void Init() override {}

    void Destroy() override {}
};

void ReleaseResourceTwice()
{
    ReviewCountedResource resource;
    resource.ReleaseReference();
    resource.ReleaseReference();
}
} // namespace

TEST(VulkanReviewTest, DoubleResourceReleaseAborts)
{
    EXPECT_DEATH(ReleaseResourceTwice(), "reference count underflow");
}

static_assert(!std::is_assignable_v<BitField<RHIAccessFlagBits>&, BitField<RHIPipelineStageFlagBits>>);
static_assert(!std::is_constructible_v<BitField<RHIAccessFlagBits>, BitField<RHIPipelineStageFlagBits>>);
static_assert(RHITexture::CalculateTextureMipLevels(0) == 1);
static_assert(RHITexture::CalculateTextureMipLevels(0, 0, 0) == 1);
static_assert(RHITexture::CalculateTextureMipLevels(17, 32, 8) == 6);

TEST(VulkanReviewTest, TextureShapeRejectsInvalidExtentsMipsAndSamples)
{
    RHITextureCreateInfo info{};
    info.type  = RHITextureType::e2D;
    info.width = info.height = 16;
    EXPECT_TRUE(IsTextureShapeValid(info));
    info.width = 0;
    EXPECT_FALSE(IsTextureShapeValid(info));
    info.width   = 16;
    info.mipmaps = 6;
    EXPECT_FALSE(IsTextureShapeValid(info));
    info.mipmaps = 2;
    info.samples = SampleCount::e4;
    EXPECT_FALSE(IsTextureShapeValid(info));
    info.mipmaps = 1;
    EXPECT_TRUE(IsTextureShapeValid(info));
    info.type        = RHITextureType::eCube;
    info.arrayLayers = 6;
    EXPECT_FALSE(IsTextureShapeValid(info));
    info.samples = SampleCount::e1;
    EXPECT_TRUE(IsTextureShapeValid(info));
    info.width = 8;
    EXPECT_FALSE(IsTextureShapeValid(info));
    info.type = RHITextureType::e1D;
    EXPECT_FALSE(IsTextureShapeValid(info));
    info.height = 1;
    EXPECT_TRUE(IsTextureShapeValid(info));
    info.type = RHITextureType::e3D;
    EXPECT_FALSE(IsTextureShapeValid(info));
    info.arrayLayers = 1;
    EXPECT_TRUE(IsTextureShapeValid(info));
}

TEST(VulkanReviewTest, ClearConversionIsNumericAndDefinedAtTheLimits)
{
    EXPECT_EQ(ToVkClearColor(Color(7, 0, 0, 0), DataFormat::eR32UInt).uint32[0], 7u);
    EXPECT_EQ(ToVkClearColor(Color(-7, 0, 0, 0), DataFormat::eR32SInt).int32[0], -7);
    EXPECT_EQ(ToVkClearColor(Color(-1, 0, 0, 0), DataFormat::eR32UInt).uint32[0], 0u);
    EXPECT_EQ(ToVkClearColor(Color(std::numeric_limits<float>::infinity()), DataFormat::eR32UInt).uint32[0], UINT32_MAX);
    EXPECT_EQ(ToVkClearColor(Color(-std::numeric_limits<float>::infinity()), DataFormat::eR32SInt).int32[0], INT32_MIN);
    EXPECT_EQ(ToVkClearColor(Color(std::numeric_limits<float>::quiet_NaN()), DataFormat::eR32UInt).uint32[0], 0u);
    EXPECT_FLOAT_EQ(ToVkClearColor(Color(0.25f), DataFormat::eR32SFloat).float32[0], 0.25f);
    const RHIRenderTargetClearValue zero;
    EXPECT_FLOAT_EQ(zero.color.r, 0.0f);
    EXPECT_FLOAT_EQ(zero.color.a, 0.0f);
}

TEST(VulkanReviewTest, BufferAllocationPolicyKeepsHostAccessAndBudgetExplicit)
{
    for (RHIBufferAllocateType type : {RHIBufferAllocateType::eGPU, RHIBufferAllocateType::eCPURead,
                                       RHIBufferAllocateType::eCPUWrite, RHIBufferAllocateType::eCPUWriteGPURead})
    {
        const VmaAllocationCreateInfo info   = MakeVmaBufferAllocation(type, false);
        const VmaAllocationCreateInfo budget = MakeVmaBufferAllocation(type, true);
        EXPECT_EQ(budget.flags, info.flags | VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT);
        const bool host = type != RHIBufferAllocateType::eGPU;
        EXPECT_EQ((info.flags & VMA_ALLOCATION_CREATE_MAPPED_BIT) != 0, host);
        EXPECT_EQ((info.requiredFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0, host);
        EXPECT_EQ(info.usage, type == RHIBufferAllocateType::eGPU || type == RHIBufferAllocateType::eCPUWriteGPURead
                                  ? VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE
                                  : VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
    }
    EXPECT_EQ(ToUnderlying(RHIBufferAllocateType::eMax), 5u);
}

TEST(VulkanReviewTest, ScalarContractsHandleBoundaryValues)
{
    EXPECT_FLOAT_EQ(ClampSamplerAnisotropy(100, 16), 16);
    EXPECT_FLOAT_EQ(ClampSamplerAnisotropy(0, 16), 1);
    EXPECT_FLOAT_EQ(ClampSamplerAnisotropy(std::numeric_limits<float>::quiet_NaN(), 16), 1);
    EXPECT_STREQ(RHIErrorText(nullptr, "fallback"), "fallback");
    EXPECT_STREQ(RHIErrorText("native", "fallback"), "native");
    EXPECT_EQ(RHIShaderStageToString(RHIShaderStage::eMax), "Unknown");
    BitField<uint64_t> bits;
    bits.SetFlags(uint64_t(1) << 40, uint64_t(1) << 2);
    EXPECT_TRUE(bits.HasAnyFlag(uint64_t(1) << 40));
    EXPECT_EQ(int64_t(bits), (int64_t(1) << 40) | 4);
}

TEST(VulkanReviewTest, LayoutRulesRejectRuntimeArraysReservedSetAndDuplicateNames)
{
    // Vulkan's guaranteed minimum, so these layouts are valid on every device.
    const uint32_t     sets = RHIGPUInfo{}.maxBoundDescriptorSets;
    RHIShaderGroupInfo info{};
    info.SRDTable.resize(2);
    RHIShaderResourceDescriptor binding{};
    binding.set     = 1;
    binding.binding = 0;
    binding.name    = "Block";
    binding.type    = RHIShaderResourceType::eUniformBuffer;
    info.SRDTable[1].push_back(binding);
    EXPECT_TRUE(ValidateShaderLayout(info, sets));
    info.SRDTable[1][0].bindless = true;
    EXPECT_FALSE(ValidateShaderLayout(info, sets));
    info.SRDTable[1][0].bindless = false;
    binding.binding              = 1;
    info.SRDTable[1].push_back(binding);
    EXPECT_FALSE(ValidateShaderLayout(info, sets));
    info.SRDTable[1].clear();
    binding.set = 0;
    info.SRDTable[0].push_back(binding);
    EXPECT_FALSE(ValidateShaderLayout(info, sets));
}

TEST(VulkanReviewTest, LayoutRulesFollowTheDeviceDescriptorSetLimit)
{
    RHIShaderGroupInfo info{};
    info.SRDTable.resize(10);
    RHIShaderResourceDescriptor binding{};
    binding.set  = 9;
    binding.name = "HighSet";
    binding.type = RHIShaderResourceType::eStorageBuffer;
    info.SRDTable[9].push_back(binding);
    // Ten sets fit a device that binds ten, and exceed one that binds nine; no engine count applies.
    EXPECT_TRUE(ValidateShaderLayout(info, 10));
    EXPECT_FALSE(ValidateShaderLayout(info, 9));
}
