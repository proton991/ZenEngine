#pragma once
#include <vulkan/vulkan.h>

namespace zen::val
{
class Device;

class Queue
{
public:
    Queue(Device& device, uint32_t queueFamilyIndex, uint32_t index, bool supportPresent);

    Queue(const Queue&) = default;

    Queue(Queue&& other);

    VkQueue GetHandle() const { return m_handle; }

private:
    VkQueue  m_handle = nullptr;
    uint32_t m_familyIndex;
    uint32_t m_index;
    bool     m_supportPresent;
};
} // namespace zen::val