#pragma once
#include <vulkan/vulkan.h>
#include "DeviceObject.h"

namespace zen::val
{
class Fence : public DeviceObject<VkFence, VK_OBJECT_TYPE_FENCE>
{
public:
    Fence(Device& device, bool signaled = false);
    ~Fence();

    bool IsSignaled() const { return m_signaled; }

private:
    bool m_signaled;
};
} // namespace zen::val