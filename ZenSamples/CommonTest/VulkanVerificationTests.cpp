#include "Graphics/VulkanRHI/VulkanCommon.h"
#include <gtest/gtest.h>

TEST(VulkanVerificationTest, SuccessAndExpectedStatusesRemainNonfatal)
{
    for (VkResult result : {VK_SUCCESS, VK_NOT_READY, VK_TIMEOUT, VK_SUBOPTIMAL_KHR})
    {
        EXPECT_FALSE(zen::MakeVulkanError(result, "expected status", __FILE__, __LINE__).IsFailure());
    }
}

TEST(VulkanVerificationTest, NativeFailurePreservesCauseAndLocation)
{
    const zen::RHIError error = zen::MakeVulkanError(VK_ERROR_OUT_OF_DEVICE_MEMORY, "test allocation", __FILE__, 42);

    EXPECT_EQ(error.code, zen::RHIErrorCode::eOutOfDeviceMemory);

    EXPECT_EQ(error.nativeCode, VK_ERROR_OUT_OF_DEVICE_MEMORY);

    EXPECT_STREQ(error.operation, "test allocation");

    EXPECT_STREQ(error.source, __FILE__);

    EXPECT_EQ(error.line, 42u);
}

TEST(VulkanVerificationTest, DeviceLossHasADistinctCode)
{
    EXPECT_EQ(zen::MakeVulkanError(VK_ERROR_DEVICE_LOST, "test device loss").code, zen::RHIErrorCode::eDeviceLost);
}
