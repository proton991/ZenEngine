#pragma once
#include <vulkan/vulkan.h>

namespace zen::val
{
class Device;
class Semaphore
{
public:
    Semaphore(Device& device);
    ~Semaphore();

    VkSemaphore GetHandle() const { return m_handle; }

private:
    Device&     m_device;
    VkSemaphore m_handle{VK_NULL_HANDLE};
};
} // namespace zen::val