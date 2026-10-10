#include "ScopedVulkanCall.h"
#include "Graphics/VulkanRHI/VulkanSynchronization.h"
#include "Graphics/VulkanRHI/VulkanTypes.h"
#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>
#include <sstream>
#include "Graphics/VulkanRHI/VulkanQueue.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanExtension.h"
#include "Graphics/VulkanRHI/VulkanCommandList.h"
#include <unordered_map>
#include <vector>
#include <type_traits>
#include <cstring>

TEST(VulkanSynchronizationTests, RayQueryEnumsMatchNativeValues)
{
    using namespace zen;
    EXPECT_EQ(static_cast<VkPipelineStageFlags>(RHIPipelineStageFlagBits::eAccelerationStructureBuild),
              VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR);
    EXPECT_EQ(static_cast<VkAccessFlags>(RHIAccessFlagBits::eAccelerationStructureRead),
              VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    EXPECT_EQ(static_cast<VkAccessFlags>(RHIAccessFlagBits::eAccelerationStructureWrite),
              VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
    EXPECT_EQ(static_cast<VkBufferUsageFlags>(RHIBufferUsageFlagBits::eDeviceAddress),
              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    EXPECT_EQ(static_cast<VkBufferUsageFlags>(RHIBufferUsageFlagBits::eAccelerationStructureInput),
              VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);
    EXPECT_EQ(static_cast<VkBufferUsageFlags>(RHIBufferUsageFlagBits::eAccelerationStructureStorage),
              VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR);
    EXPECT_EQ(static_cast<VkAccelerationStructureTypeKHR>(RHIAccelerationStructureType::eTopLevel),
              VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR);
    EXPECT_EQ(static_cast<VkAccelerationStructureTypeKHR>(RHIAccelerationStructureType::eBottomLevel),
              VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);
    EXPECT_EQ(static_cast<VkGeometryInstanceFlagsKHR>(RHIAccelerationStructureInstanceFlagBits::eDisableTriangleCulling),
              VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR);
    EXPECT_EQ(static_cast<VkGeometryInstanceFlagsKHR>(RHIAccelerationStructureInstanceFlagBits::eReverseTriangleFacing),
              VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR);
    EXPECT_EQ(static_cast<VkGeometryInstanceFlagsKHR>(RHIAccelerationStructureInstanceFlagBits::eForceOpaque),
              VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR);
    EXPECT_EQ(static_cast<VkGeometryInstanceFlagsKHR>(RHIAccelerationStructureInstanceFlagBits::eForceNonOpaque),
              VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR);

    RHIAccelerationStructureInstance instance;
    instance.transform[0][0] = instance.transform[1][1] = instance.transform[2][2] = 1.0f;
    instance.customIndexAndMask                                                    = 7u | (0xffu << 24);
    instance.offsetAndFlags = ToUnderlying(RHIAccelerationStructureInstanceFlagBits::eDisableTriangleCulling) << 24;
    instance.accelerationStructureAddress = 4096;
    VkAccelerationStructureInstanceKHR native{};
    static_assert(sizeof(instance) == sizeof(native));
    std::memcpy(&native, &instance, sizeof(native));
    EXPECT_EQ(native.instanceCustomIndex, 7u);
    EXPECT_EQ(native.mask, 0xffu);
    EXPECT_EQ(native.instanceShaderBindingTableRecordOffset, 0u);
    EXPECT_EQ(native.flags, VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR);
    EXPECT_EQ(native.accelerationStructureReference, 4096u);
}

TEST(VulkanSynchronizationTests, NativeBufferFlagsKeepBuildAndShaderAccessesDistinct)
{
    using namespace zen;
    BitField<RHIBufferUsageFlagBits> usage;
    usage.SetFlags(RHIBufferUsageFlagBits::eTransferDstBuffer, RHIBufferUsageFlagBits::eStorageBuffer,
                   RHIBufferUsageFlagBits::eAccelerationStructureInput, RHIBufferUsageFlagBits::eDeviceAddress);
    EXPECT_EQ(usage, VkBufferUsageFlags(VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                                        | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
                                        | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT));
    const BitField<RHIBufferUsageFlagBits>   storage(RHIBufferUsageFlagBits::eStorageBuffer);
    const BitField<RHIPipelineStageFlagBits> build(RHIPipelineStageFlagBits::eAccelerationStructureBuild);
    const BitField<RHIPipelineStageFlagBits> compute(RHIPipelineStageFlagBits::eComputeShader);
    EXPECT_EQ(RHIBufferUsageToAccessFlagBits(storage, RHIAccessMode::eReadWrite, build),
              VkAccessFlags(VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR));
    EXPECT_EQ(RHIBufferUsageToAccessFlagBits(storage, RHIAccessMode::eReadWrite, compute),
              VkAccessFlags(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT));
    // A broad stage on an ordinary storage buffer must also work with RT disabled.
    const BitField<RHIPipelineStageFlagBits> allCommands(RHIPipelineStageFlagBits::eAllCommands);
    EXPECT_EQ(RHIBufferUsageToAccessFlagBits(storage, RHIAccessMode::eReadWrite, allCommands),
              VkAccessFlags(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT));
    const BitField<RHIPipelineStageFlagBits> combined(int64_t(build) | int64_t(compute));
    EXPECT_EQ(RHIBufferUsageToAccessFlagBits(storage, RHIAccessMode::eReadWrite, combined),
              VkAccessFlags(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
                            | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR));
    EXPECT_EQ(
        RHIBufferUsageToAccessFlagBits(BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eAccelerationStructureInput),
                                       RHIAccessMode::eRead, build),
        VkAccessFlags(VK_ACCESS_SHADER_READ_BIT));
    EXPECT_EQ(
        RHIBufferUsageToAccessFlagBits(BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eAccelerationStructureStorage),
                                       RHIAccessMode::eRead, compute),
        VkAccessFlags(VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR));
}

// Specify expected Vulkan bits directly so a shared RHI conversion bug cannot
// make both the barrier and its expected mask wrong in the same way.
TEST(VulkanSynchronizationTests, ColorAttachmentMasksCoverLoadAndBlendReads)
{
    using namespace zen;

    EXPECT_EQ(RHITextureUsageToAccessFlagBits(RHITextureUsage::eColorAttachment, RHIAccessMode::eReadWrite),
              VkAccessFlags(VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT));

    EXPECT_EQ(RHITextureUsageToAccessFlagBits(RHITextureUsage::eColorAttachment, RHIAccessMode::eRead),
              VkAccessFlags(VK_ACCESS_COLOR_ATTACHMENT_READ_BIT));

    EXPECT_EQ(RHITextureUsageToAccessFlagBits(RHITextureUsage::eColorAttachment, RHIAccessMode::eNone), VkAccessFlags(0));
}

TEST(VulkanSynchronizationTests, DepthStencilAttachmentMasksCoverLoadAndTestReads)
{
    using namespace zen;

    EXPECT_EQ(RHITextureUsageToAccessFlagBits(RHITextureUsage::eDepthStencilAttachment, RHIAccessMode::eReadWrite),
              VkAccessFlags(VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT));

    EXPECT_EQ(RHITextureUsageToAccessFlagBits(RHITextureUsage::eDepthStencilAttachment, RHIAccessMode::eRead),
              VkAccessFlags(VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT));

    EXPECT_EQ(RHITextureUsageToAccessFlagBits(RHITextureUsage::eDepthStencilAttachment, RHIAccessMode::eNone),
              VkAccessFlags(0));
}

TEST(VulkanSynchronizationTests, UploadToUniformBufferUsesUniformReadAccess)
{
    using namespace zen;

    EXPECT_EQ(RHIBufferUsageToAccessFlagBits(RHIBufferUsage::eTransferDst, RHIAccessMode::eReadWrite),
              VkAccessFlags(VK_ACCESS_TRANSFER_WRITE_BIT));

    EXPECT_EQ(RHIBufferUsageToAccessFlagBits(RHIBufferUsage::eUniformBuffer, RHIAccessMode::eRead),
              VkAccessFlags(VK_ACCESS_UNIFORM_READ_BIT));

    // A uniform texel buffer still uses shader-read access.
    EXPECT_EQ(RHIBufferUsageToAccessFlagBits(RHIBufferUsage::eTextureBuffer, RHIAccessMode::eRead),
              VkAccessFlags(VK_ACCESS_SHADER_READ_BIT));
}

TEST(VulkanSynchronizationTests, ReleasingUnusedSemaphoreSlotsDoesNotPopulateThePool)
{
    std::ostringstream errors;

    // spdlog's public logger/sink APIs require std::shared_ptr.
    std::shared_ptr<spdlog::sinks::ostream_sink<std::mutex>> sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(errors);

    std::shared_ptr<spdlog::logger> logger                        = std::make_shared<spdlog::logger>("semaphore_test", sink);

    std::shared_ptr<spdlog::logger> previousLogger                = spdlog::default_logger();

    spdlog::set_default_logger(logger);

    zen::VulkanSemaphoreManager manager(nullptr);

    zen::VulkanSemaphore* semaphore = nullptr;

    manager.ReleaseSemaphore(semaphore);

    manager.ReleaseSemaphore(semaphore);

    manager.Destroy();

    manager.Destroy();

    spdlog::set_default_logger(previousLogger);

    EXPECT_EQ(semaphore, nullptr);

    EXPECT_TRUE(errors.str().empty()) << errors.str();
}

namespace
{
using zen::test::ScopedVulkanCall;
TEST(VulkanNameTests, InternedLayerNamesPreserveGlobalEnumerationAndCallerStorage)
{
    using namespace zen;

    static uint32_t queries = 0;

    queries                 = 0;

    ScopedVulkanCall<PFN_vkEnumerateInstanceExtensionProperties> enumerateExtensions(
        vkEnumerateInstanceExtensionProperties, [](const char* layer, uint32_t* count, VkExtensionProperties*) -> VkResult {
            if (queries == 0)
            {
                EXPECT_EQ(layer, nullptr);
            }
            else
            {
                EXPECT_STREQ(layer, "VK_LAYER_KHRONOS_validation");
            }

            ++queries;

            *count = 0;

            return VK_SUCCESS;
        });

    EXPECT_TRUE(VulkanInstanceExtension::GetSupportedInstanceExtensions().empty());

    std::string layerText = "VK_LAYER_KHRONOS_validation";

    const NameID layerName(layerText);

    layerText.clear();

    EXPECT_TRUE(VulkanInstanceExtension::GetSupportedInstanceExtensions(layerName).empty());

    EXPECT_EQ(queries, 2u);

    std::string extensionText = "VK_EXT_debug_utils";

    VulkanInstanceExtension extension{NameID(extensionText)};

    extensionText.clear();

    EXPECT_EQ(extension.GetName(), NameID("VK_EXT_debug_utils"));
}

} // namespace
