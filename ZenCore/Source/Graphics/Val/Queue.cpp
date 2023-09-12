#include "Graphics/Val/Queue.h"
#include "Graphics/Val/Device.h"

namespace zen::val
{

Queue::Queue(Device& device, uint32_t queueFamilyIndex, uint32_t index, bool supportPresent) :
    m_familyIndex(queueFamilyIndex), m_index(index), m_supportPresent(supportPresent)
{
    vkGetDeviceQueue(device.GetHandle(), m_familyIndex, m_index, &m_handle);
}

Queue::Queue(Queue&& other) :
    m_handle(other.m_handle), m_familyIndex(other.m_familyIndex), m_index(other.m_index), m_supportPresent(other.m_supportPresent)
{
    other.m_handle         = VK_NULL_HANDLE;
    other.m_familyIndex    = {};
    other.m_index          = {};
    other.m_supportPresent = VK_FALSE;
}
} // namespace zen::val