#include "Graphics/Val/Queue.h"

namespace zen::val
{

Queue::Queue(Device& device, uint32_t queueFamilyIndex, uint32_t index, bool supportPresent) :
    DeviceObject(device), m_familyIndex(queueFamilyIndex), m_index(index), m_supportPresent(supportPresent)
{
    vkGetDeviceQueue(device.GetHandle(), m_familyIndex, m_index, &m_handle);
}
} // namespace zen::val