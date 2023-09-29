#include "Graphics/Val/Buffer.h"
#include "Common/Errors.h"
#include "Graphics/Val/VulkanDebug.h"

namespace zen::val
{
SharedPtr<Buffer> Buffer::Create(Device& device, const BufferCreateInfo& CI)
{
    return MakeShared<Buffer>(device, CI.size, CI.usage, CI.vmaFlags, "");
}

UniquePtr<Buffer> Buffer::CreateUnique(Device& device, const BufferCreateInfo& CI)
{
    return MakeUnique<Buffer>(device, CI.size, CI.usage, CI.vmaFlags, "");
}

Buffer::Buffer(Device& device, VkDeviceSize size, VkBufferUsageFlags usage, VmaAllocationCreateFlags vmaFlags, std::string debugName) :
    DeviceObject(device, debugName), m_size(size)
{
    VkBufferCreateInfo bufferCI{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferCI.usage       = usage;
    bufferCI.size        = size;
    bufferCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;


    VmaAllocationCreateInfo vmaAllocCI{};
    vmaAllocCI.usage = VMA_MEMORY_USAGE_AUTO;
    vmaAllocCI.flags = vmaFlags;

    CHECK_VK_ERROR_AND_THROW(vmaCreateBuffer(m_device.GetAllocator(), &bufferCI,
                                             &vmaAllocCI, &m_handle, &m_allocation,
                                             nullptr),
                             "vmaCreateBuffer failed");
    SetBufferName(m_device.GetHandle(), m_handle, debugName.data());
}

Buffer::~Buffer()
{
    if (m_handle != VK_NULL_HANDLE && m_allocation != nullptr)
    {
        vmaDestroyBuffer(m_device.GetAllocator(), m_handle, m_allocation);
    }
}

VkAccessFlags Buffer::UsageToAccessFlags(VkBufferUsageFlags usage)
{
    if (usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT)
        return VK_ACCESS_TRANSFER_READ_BIT;
    if (usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT)
        return VK_ACCESS_TRANSFER_WRITE_BIT;
    if (usage & (VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT))
        return VK_ACCESS_SHADER_READ_BIT;
    if (usage & (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT))
        return VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    if (usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT)
        return VK_ACCESS_INDEX_READ_BIT;
    if (usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)
        return VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    return VkAccessFlags{};
}

VkPipelineStageFlags Buffer::UsageToPipelineStage(VkBufferUsageFlags usage)
{
    if (usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT)
        return VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT)
        return VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (usage & (VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT))
        return VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
    if (usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)
        return VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
    if (usage & (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT))
        return VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    if (usage & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT)
        return VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
    if (usage & VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT)
        return VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    return VkPipelineStageFlags{};
}

} // namespace zen::val