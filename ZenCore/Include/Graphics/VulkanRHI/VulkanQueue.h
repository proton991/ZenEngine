#pragma once
#include "VulkanHeaders.h"

namespace zen
{
class VulkanDevice;

class VulkanQueue
{
public:
    VulkanQueue(VulkanDevice* device, uint32_t familyIndex);

    uint32_t GetFamilyIndex() const { return m_familyIndex; }

    uint32_t GetQueueIndex() const { return m_queueIndex; }

    VkQueue GetHandle() const { return m_handle; }

private:
    VulkanDevice* m_device{nullptr};
    VkQueue       m_handle{VK_NULL_HANDLE};
    uint32_t      m_familyIndex;
    uint32_t      m_queueIndex;
};
} // namespace zen