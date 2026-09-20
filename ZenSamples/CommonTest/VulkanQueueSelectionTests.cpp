#include "Graphics/VulkanRHI/VulkanQueueSelection.h"
#include <gtest/gtest.h>
#include "Templates/HeapVector.h"

namespace
{
using namespace zen;
constexpr VkQueueFlags kGraphicsCompute = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;

VkQueueFamilyProperties Family(VkQueueFlags flags, uint32_t count = 1)
{
    VkQueueFamilyProperties properties{};
    properties.queueFlags = flags;
    properties.queueCount = count;
    return properties;
}

void CheckQueue(const VulkanQueueLocation& queue, uint32_t family, uint32_t index)
{
    EXPECT_EQ(queue.familyIndex, family);
    EXPECT_EQ(queue.queueIndex, index);
}

TEST(VulkanQueueSelectionTests, SelectsDedicatedComputeAndTransferRegardlessOfFamilyOrder)
{
    const HeapVector<VkQueueFamilyProperties> families{
        Family(VK_QUEUE_TRANSFER_BIT), Family(VK_QUEUE_COMPUTE_BIT), Family(kGraphicsCompute, 4)};
    const VulkanQueueSelection selection = SelectVulkanQueues(families);
    ASSERT_TRUE(selection.IsValid());
    CheckQueue(selection.graphics, 2, 0);
    CheckQueue(selection.compute, 1, 0);
    CheckQueue(selection.transfer, 0, 0);
    for (uint32_t family = 0; family < families.size(); ++family)
    {
        EXPECT_EQ(selection.GetRequestedQueueCount(family), 1u);
        EXPECT_LE(selection.GetRequestedQueueCount(family), families[family].queueCount);
    }
}

TEST(VulkanQueueSelectionTests, UsesSecondQueueInGraphicsFamilyAndSharesItWithTransfer)
{
    const HeapVector<VkQueueFamilyProperties> families{Family(kGraphicsCompute, 2)};
    const VulkanQueueSelection selection = SelectVulkanQueues(families);
    ASSERT_TRUE(selection.IsValid());
    CheckQueue(selection.graphics, 0, 0);
    CheckQueue(selection.compute, 0, 1);
    CheckQueue(selection.transfer, 0, 1);
    EXPECT_EQ(selection.GetRequestedQueueCount(0), 2u);
    EXPECT_EQ(selection.GetRequestedQueueCount(1), 0u);
}

TEST(VulkanQueueSelectionTests, SingleAvailableQueueAliasesAllContexts)
{
    const HeapVector<VkQueueFamilyProperties> families{Family(kGraphicsCompute)};
    const VulkanQueueSelection selection = SelectVulkanQueues(families);
    ASSERT_TRUE(selection.IsValid());
    CheckQueue(selection.graphics, 0, 0);
    CheckQueue(selection.compute, 0, 0);
    CheckQueue(selection.transfer, 0, 0);
    EXPECT_EQ(selection.GetRequestedQueueCount(0), 1u);
}

TEST(VulkanQueueSelectionTests, SeparateComputeFamilyNeedNotBeComputeOnly)
{
    const HeapVector<VkQueueFamilyProperties> families{Family(kGraphicsCompute, 3),
                                                       Family(kGraphicsCompute)};
    const VulkanQueueSelection selection = SelectVulkanQueues(families);
    ASSERT_TRUE(selection.IsValid());
    CheckQueue(selection.compute, 1, 0);
    CheckQueue(selection.transfer, 1, 0);
    EXPECT_EQ(selection.GetRequestedQueueCount(0), 1u);
    EXPECT_EQ(selection.GetRequestedQueueCount(1), 1u);
}

TEST(VulkanQueueSelectionTests, PrefersComputeOnlyFamilyAndIgnoresEmptyFamilies)
{
    const HeapVector<VkQueueFamilyProperties> families{
        Family(kGraphicsCompute, 0),  Family(kGraphicsCompute, 2),
        Family(kGraphicsCompute),     Family(VK_QUEUE_COMPUTE_BIT, 0),
        Family(VK_QUEUE_COMPUTE_BIT), Family(VK_QUEUE_TRANSFER_BIT, 0)};
    const VulkanQueueSelection selection = SelectVulkanQueues(families);
    ASSERT_TRUE(selection.IsValid());
    CheckQueue(selection.graphics, 1, 0);
    CheckQueue(selection.compute, 4, 0);
    CheckQueue(selection.transfer, 4, 0);
    EXPECT_EQ(selection.GetRequestedQueueCount(0), 0u);
    EXPECT_EQ(selection.GetRequestedQueueCount(2), 0u);
    EXPECT_EQ(selection.GetRequestedQueueCount(3), 0u);
    EXPECT_EQ(selection.GetRequestedQueueCount(5), 0u);
}

TEST(VulkanQueueSelectionTests, SameFamilyComputeKeepsDedicatedTransfer)
{
    const HeapVector<VkQueueFamilyProperties> families{Family(kGraphicsCompute, 2),
                                                       Family(VK_QUEUE_TRANSFER_BIT)};
    const VulkanQueueSelection selection = SelectVulkanQueues(families);
    ASSERT_TRUE(selection.IsValid());
    CheckQueue(selection.compute, 0, 1);
    CheckQueue(selection.transfer, 1, 0);
    EXPECT_EQ(selection.GetRequestedQueueCount(0), 2u);
    EXPECT_EQ(selection.GetRequestedQueueCount(1), 1u);
}

TEST(VulkanQueueSelectionTests, MissingGraphicsComputeFamilyRejectsSelection)
{
    const HeapVector<VkQueueFamilyProperties> empty;
    const HeapVector<VkQueueFamilyProperties> unsupported{
        Family(VK_QUEUE_GRAPHICS_BIT), Family(VK_QUEUE_COMPUTE_BIT), Family(kGraphicsCompute, 0)};
    EXPECT_FALSE(SelectVulkanQueues(empty).IsValid());
    const VulkanQueueSelection selection = SelectVulkanQueues(unsupported);
    EXPECT_FALSE(selection.IsValid());
    EXPECT_EQ(selection.GetRequestedQueueCount(0), 0u);
    EXPECT_EQ(selection.GetRequestedQueueCount(UINT32_MAX), 0u);
}
} // namespace
