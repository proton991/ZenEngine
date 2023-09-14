#pragma once
#include <vulkan/vulkan.h>
#include "DeviceObject.h"

namespace zen::val
{
class Queue : public DeviceObject<VkQueue, VK_OBJECT_TYPE_QUEUE>
{
public:
    Queue(Device& device, uint32_t queueFamilyIndex, uint32_t index, bool supportPresent);
    
private:
    uint32_t m_familyIndex;
    uint32_t m_index;
    bool     m_supportPresent;
};
} // namespace zen::val