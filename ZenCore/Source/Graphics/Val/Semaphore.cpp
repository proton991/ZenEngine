#include "Graphics/Val/Semaphore.h"
#include "Graphics/Val/Device.h"
#include "Common/Errors.h"

namespace zen::val
{
Semaphore::Semaphore(Device& device) :
    m_device(device)
{
    VkSemaphoreCreateInfo semaphoreCI{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    CHECK_VK_ERROR_AND_THROW(vkCreateSemaphore(m_device.GetHandle(), &semaphoreCI, nullptr, &m_handle), "Failed to create semaphore");
}

Semaphore::~Semaphore()
{
    if (m_handle != VK_NULL_HANDLE)
    {
        vkDestroySemaphore(m_device.GetHandle(), m_handle, nullptr);
    }
}
} // namespace zen::val