#pragma once
#include <vulkan/vulkan.h>
#include "DeviceObject.h"

namespace zen::val
{
class Queue : public DeviceObject<VkQueue, VK_OBJECT_TYPE_QUEUE>
{
public:
    Queue(const Device& device, uint32_t queueFamilyIndex, uint32_t index, bool supportPresent);

    auto GetFamilyIndex() const
    {
        return m_familyIndex;
    }

    VkResult Submit(const std::vector<VkSubmitInfo>& submitInfos, VkFence fence) const;

    VkResult Present(const VkPresentInfoKHR* info) const;

private:
    uint32_t m_familyIndex;
    uint32_t m_index;
    bool m_supportPresent;
};
} // namespace zen::val