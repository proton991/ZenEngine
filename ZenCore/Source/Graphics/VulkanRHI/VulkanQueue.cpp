#include "Graphics/VulkanRHI/VulkanQueue.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"

namespace zen::rhi
{
VulkanQueue::VulkanQueue(VulkanDevice* device, uint32_t familyIndex) :
    m_device(device), m_familyIndex(familyIndex), m_queueIndex(0)
{
    vkGetDeviceQueue(device->GetVkHandle(), m_familyIndex, m_queueIndex, &m_handle);
}


} // namespace zen::rhi