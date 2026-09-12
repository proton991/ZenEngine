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


// Specify expected Vulkan bits directly so a shared RHI conversion bug cannot
// make both the barrier and its expected mask wrong in the same way.
TEST(VulkanSynchronizationTests, ColorAttachmentMasksCoverLoadAndBlendReads)
{
    using namespace zen;
    EXPECT_EQ(
        ToVkAccessFlags(RHITextureUsageToAccessFlagBits(RHITextureUsage::eColorAttachment,
                                                        RHIAccessMode::eReadWrite)),
        VkAccessFlags(VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT));
    EXPECT_EQ(ToVkAccessFlags(RHITextureUsageToAccessFlagBits(RHITextureUsage::eColorAttachment,
                                                              RHIAccessMode::eRead)),
              VkAccessFlags(VK_ACCESS_COLOR_ATTACHMENT_READ_BIT));
    EXPECT_EQ(ToVkAccessFlags(RHITextureUsageToAccessFlagBits(RHITextureUsage::eColorAttachment,
                                                              RHIAccessMode::eNone)),
              VkAccessFlags(0));
}

TEST(VulkanSynchronizationTests, DepthStencilAttachmentMasksCoverLoadAndTestReads)
{
    using namespace zen;
    EXPECT_EQ(ToVkAccessFlags(RHITextureUsageToAccessFlagBits(
                  RHITextureUsage::eDepthStencilAttachment, RHIAccessMode::eReadWrite)),
              VkAccessFlags(VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT));
    EXPECT_EQ(ToVkAccessFlags(RHITextureUsageToAccessFlagBits(
                  RHITextureUsage::eDepthStencilAttachment, RHIAccessMode::eRead)),
              VkAccessFlags(VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT));
    EXPECT_EQ(ToVkAccessFlags(RHITextureUsageToAccessFlagBits(
                  RHITextureUsage::eDepthStencilAttachment, RHIAccessMode::eNone)),
              VkAccessFlags(0));
}

TEST(VulkanSynchronizationTests, UploadToUniformBufferUsesUniformReadAccess)
{
    using namespace zen;
    EXPECT_EQ(ToVkAccessFlags(RHIBufferUsageToAccessFlagBits(RHIBufferUsage::eTransferDst,
                                                             RHIAccessMode::eReadWrite)),
              VkAccessFlags(VK_ACCESS_TRANSFER_WRITE_BIT));
    EXPECT_EQ(ToVkAccessFlags(RHIBufferUsageToAccessFlagBits(RHIBufferUsage::eUniformBuffer,
                                                             RHIAccessMode::eRead)),
              VkAccessFlags(VK_ACCESS_UNIFORM_READ_BIT));
    // A uniform texel buffer still uses shader-read access.
    EXPECT_EQ(ToVkAccessFlags(RHIBufferUsageToAccessFlagBits(RHIBufferUsage::eTextureBuffer,
                                                             RHIAccessMode::eRead)),
              VkAccessFlags(VK_ACCESS_SHADER_READ_BIT));
}

TEST(VulkanSynchronizationTests, ReleasingUnusedSemaphoreSlotsDoesNotPopulateThePool)
{
    std::ostringstream errors;
    std::shared_ptr<spdlog::sinks::ostream_sink<std::mutex>> sink =
        std::make_shared<spdlog::sinks::ostream_sink_mt>(errors);
    std::shared_ptr<spdlog::logger> logger =
        std::make_shared<spdlog::logger>("semaphore_test", sink);
    std::shared_ptr<spdlog::logger> previousLogger = spdlog::default_logger();
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
        vkEnumerateInstanceExtensionProperties,
        [](const char* layer, uint32_t* count, VkExtensionProperties*) -> VkResult {
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
