#pragma once
#include "Graphics/RHI/RHIResource.h"
#include "VulkanHeaders.h"
#include "VulkanMemory.h"


namespace zen::rhi
{
// struct VulkanBuffer
// {
//     VkBuffer buffer{VK_NULL_HANDLE};
//     uint32_t allocatedSize{0};
//     uint32_t requiredSize{0};
//     VkBufferView bufferView{VK_NULL_HANDLE};
//     VulkanMemoryAllocation memAlloc{};
// };

class VulkanBuffer : public RHIBuffer
{
public:
    static VulkanBuffer* CreateObject(const RHIBufferCreateInfo& createInfo);

    uint8_t* Map() override;

    void Unmap() override;

    void SetTexelFormat(DataFormat format) override;

    VkBuffer GetVkBuffer() const
    {
        return m_vkBuffer;
    }

    VkBufferView GetVkBufferView() const
    {
        return m_bufferView;
    }

protected:
    void Init() override;

    void Destroy() override;

private:
    explicit VulkanBuffer(const RHIBufferCreateInfo& createInfo) : RHIBuffer(createInfo) {}

    VkBuffer m_vkBuffer{VK_NULL_HANDLE};
    uint32_t m_allocatedSize{0};
    VkBufferView m_bufferView{VK_NULL_HANDLE};
    VulkanMemoryAllocation m_memAlloc{};
};
} // namespace zen::rhi