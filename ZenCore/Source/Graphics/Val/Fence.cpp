#include "Graphics/Val/Fence.h"
#include "Graphics/Val/Device.h"
#include "Common/Errors.h"

namespace zen::val
{
Fence::Fence(Device& device, bool signaled) :
    m_device(device), m_signaled(signaled)
{
    VkFenceCreateInfo fenceCI{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceCI.flags = signaled ? VK_FENCE_CREATE_SIGNALED_BIT : 0;
    CHECK_VK_ERROR_AND_THROW(vkCreateFence(m_device.GetHandle(), &fenceCI, nullptr, &m_handle), "Failed to create fence");
}

Fence::~Fence()
{
    if (m_handle != VK_NULL_HANDLE)
    {
        vkDestroyFence(m_device.GetHandle(), m_handle, nullptr);
    }
}
} // namespace zen::val