#include "Graphics/Val/Buffer.h"
#include "Common/Errors.h"
#include "Graphics/Val/VulkanDebug.h"

namespace zen::val
{
SharedPtr<Buffer> Buffer::Create(const Device& device, const BufferCreateInfo& CI)
{
    return MakeShared<Buffer>(device, CI.byteSize, CI.usage, CI.vmaFlags);
}

UniquePtr<Buffer> Buffer::CreateUnique(const Device& device, const BufferCreateInfo& CI)
{
    return MakeUnique<Buffer>(device, CI.byteSize, CI.usage, CI.vmaFlags);
}

Buffer::Buffer(const Device&            device,
               VkDeviceSize             byteSize,
               VkBufferUsageFlags       usage,
               VmaAllocationCreateFlags vmaFlags) :
    DeviceObject(device), m_byteSize(byteSize)
{
    VkBufferCreateInfo bufferCI{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferCI.usage       = usage;
    bufferCI.size        = byteSize;
    bufferCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;


    VmaAllocationCreateInfo vmaAllocCI{};
    vmaAllocCI.usage = VMA_MEMORY_USAGE_AUTO;
    vmaAllocCI.flags = vmaFlags;

    CHECK_VK_ERROR_AND_THROW(vmaCreateBuffer(m_device.GetAllocator(), &bufferCI, &vmaAllocCI,
                                             &m_handle, &m_allocation, nullptr),
                             "vmaCreateBuffer failed");
}

Buffer::~Buffer()
{
    if (m_handle != VK_NULL_HANDLE && m_allocation != nullptr)
    {
        if (m_mappedMemory != nullptr) { UnmapMemory(); }
        vmaDestroyBuffer(m_device.GetAllocator(), m_handle, m_allocation);
        m_handle = VK_NULL_HANDLE;
    }
}

VkAccessFlags Buffer::UsageToAccessFlags(VkBufferUsageFlags usage)
{
    if (usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT) return VK_ACCESS_TRANSFER_READ_BIT;
    if (usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) return VK_ACCESS_TRANSFER_WRITE_BIT;
    if (usage & (VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT))
        return VK_ACCESS_SHADER_READ_BIT;
    if (usage & (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT))
        return VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    if (usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) return VK_ACCESS_INDEX_READ_BIT;
    if (usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) return VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    return VkAccessFlags{};
}

VkPipelineStageFlags Buffer::UsageToPipelineStage(VkBufferUsageFlags usage)
{
    if (usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT) return VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) return VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (usage & (VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT))
        return VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
    if (usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) return VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
    if (usage & (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT))
        return VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    if (usage & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT) return VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
    if (usage & VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT)
        return VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    return VkPipelineStageFlags{};
}

bool Buffer::IsMemoryMapped() const { return m_mappedMemory != nullptr; }

uint8_t* Buffer::MapMemory()
{
    if (m_mappedMemory == nullptr)
    {
        void* mem = nullptr;
        vmaMapMemory(m_device.GetAllocator(), m_allocation, &mem);
        m_mappedMemory = static_cast<uint8_t*>(mem);
    }
    return m_mappedMemory;
}

void Buffer::UnmapMemory()
{
    vmaUnmapMemory(m_device.GetAllocator(), m_allocation);
    m_mappedMemory = nullptr;
}

void Buffer::FlushMemory(size_t byteSize, size_t offset)
{
    vmaFlushAllocation(m_device.GetAllocator(), m_allocation, offset, byteSize);
}

void Buffer::CopyData(const uint8_t* data, size_t byteSize, size_t offset)
{
    ASSERT(byteSize + offset <= m_byteSize);
    if (m_mappedMemory == nullptr)
    {
        MapMemory();
        std::memcpy(static_cast<void*>(m_mappedMemory + offset), static_cast<const void*>(data),
                    byteSize);
        FlushMemory(byteSize, offset);
        UnmapMemory();
    }
    else
    {
        std::memcpy(static_cast<void*>(m_mappedMemory + offset), static_cast<const void*>(data),
                    byteSize);
    }
}
} // namespace zen::val