#pragma once
#include <vulkan/vulkan.h>

namespace zen::val
{
class Device;

class Fence
{
public:
    Fence(Device& device, bool signaled = false);
    ~Fence();

    VkFence GetHandle() const { return m_handle; }
    bool    IsSignaled() const { return m_signaled; }

private:
    Device& m_device;
    VkFence m_handle{VK_NULL_HANDLE};
    bool    m_signaled;
};
} // namespace zen::val