#include "Graphics/Val/Queue.h"
#include "Common/Helpers.h"

namespace zen::val
{

Queue::Queue(const Device& device, uint32_t queueFamilyIndex, uint32_t index, bool supportPresent) :
    DeviceObject(device),
    m_familyIndex(queueFamilyIndex),
    m_index(index),
    m_supportPresent(supportPresent)
{
    vkGetDeviceQueue(device.GetHandle(), m_familyIndex, m_index, &m_handle);
}

VkResult Queue::Submit(const std::vector<VkSubmitInfo>& submitInfos, VkFence fence) const
{
    return vkQueueSubmit(m_handle, util::ToU32(submitInfos.size()), submitInfos.data(), fence);
}

VkResult Queue::Present(const VkPresentInfoKHR* info) const
{
    return vkQueuePresentKHR(m_handle, info);
}
} // namespace zen::val