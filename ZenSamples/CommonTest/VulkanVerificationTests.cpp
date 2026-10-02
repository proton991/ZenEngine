#include "Graphics/VulkanRHI/VulkanCommon.h"
#include <gtest/gtest.h>

TEST(VulkanVerificationTest, SuccessAndExpectedStatusesRemainNonfatal)
{
    int evaluations = 0;

    VKCHECK((++evaluations, VK_SUCCESS));
    VKCHECK(VK_NOT_READY);
    VKCHECK(VK_TIMEOUT);
    VKCHECK(VK_SUBOPTIMAL_KHR);

    EXPECT_EQ(evaluations, 1);
}

TEST(VulkanVerificationDeathTest, NativeFailureStopsExecution)
{
    EXPECT_DEATH(VKCHECK(VK_ERROR_OUT_OF_DEVICE_MEMORY), "VK_ERROR_OUT_OF_DEVICE_MEMORY");
}

TEST(VulkanVerificationDeathTest, DeviceLossStopsExecution)
{
    EXPECT_DEATH(VKCHECK(VK_ERROR_DEVICE_LOST), "VK_ERROR_DEVICE_LOST");
}
