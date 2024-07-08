#include "Graphics/Val/Buffer.h"
#include "Common/Errors.h"
#include "Graphics/Val/VulkanDebug.h"

namespace zen::val
{
SharedPtr<Buffer> Buffer::Create(const Device& device, const BufferCreateInfo& CI)
{
    return MakeShared<Buffer>(device, CI.byteSize, (VkBufferUsageFlags)CI.usage, CI.vmaFlags);
}

UniquePtr<Buffer> Buffer::CreateUnique(const Device& device, const BufferCreateInfo& CI)
{
    return MakeUnique<Buffer>(device, CI.byteSize, (VkBufferUsageFlags)CI.usage, CI.vmaFlags);
}

Buffer::Buffer(const Device& device,
               VkDeviceSize byteSize,
               VkBufferUsageFlags usage,
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
        if (m_mappedMemory != nullptr)
        {
            UnmapMemory();
        }
        vmaDestroyBuffer(m_device.GetAllocator(), m_handle, m_allocation);
        m_handle = VK_NULL_HANDLE;
    }
}

VkAccessFlags Buffer::UsageToAccessFlags(BufferUsage usage)
{
    switch (usage)
    {
        case BufferUsage::Undefined: break;
        case BufferUsage::TransferSrc: return VK_ACCESS_TRANSFER_READ_BIT;
        case BufferUsage::TransferDst: return VK_ACCESS_TRANSFER_WRITE_BIT;

        case BufferUsage::UniformTexelBuffer:
        case BufferUsage::UniformBuffer:
        case BufferUsage::ShaderDeviceAddress:
        case BufferUsage::ShaderBindingTable: return VK_ACCESS_SHADER_READ_BIT;

        case BufferUsage::StorageTexelBuffer:
        case BufferUsage::StorageBuffer:
            return VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

        case BufferUsage::IndexBuffer: return VK_ACCESS_INDEX_READ_BIT;
        case BufferUsage::VertexBuffer: return VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        case BufferUsage::IndirectBuffer: return VK_ACCESS_INDIRECT_COMMAND_READ_BIT;

        case BufferUsage::TransformFeedbackBuffer:
            return VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT;

        case BufferUsage::TransformFeedbackCounterBuffer:
            return VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT;

        case BufferUsage::ConditionalRendering: return VK_ACCESS_CONDITIONAL_RENDERING_READ_BIT_EXT;

        case BufferUsage::AccelerationStructureBuildInputReadOnly:
            return VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;

        case BufferUsage::AccelerationStructureStorage:
            return VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        default: break;
    }
    return VkAccessFlags{};
}

VkPipelineStageFlags Buffer::UsageToPipelineStage(BufferUsage usage)
{
    switch (usage)
    {
        case BufferUsage::Undefined: return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

        case BufferUsage::TransferSrc:
        case BufferUsage::TransferDst: return VK_PIPELINE_STAGE_TRANSFER_BIT;

        case BufferUsage::UniformTexelBuffer:
            return VK_PIPELINE_STAGE_VERTEX_SHADER_BIT; // TODO: from other shader stages?

        case BufferUsage::StorageTexelBuffer:
        case BufferUsage::StorageBuffer: return VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

        case BufferUsage::UniformBuffer: return VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;

        case BufferUsage::IndexBuffer: return VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;

        case BufferUsage::VertexBuffer: return VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;

        case BufferUsage::IndirectBuffer: return VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;

        case BufferUsage::ShaderDeviceAddress:
            return VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; // TODO: what should be here?

        case BufferUsage::TransformFeedbackBuffer:
        case BufferUsage::TransformFeedbackCounterBuffer:
            return VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT;

        case BufferUsage::ConditionalRendering:
            return VK_PIPELINE_STAGE_CONDITIONAL_RENDERING_BIT_EXT;

        case BufferUsage::AccelerationStructureBuildInputReadOnly:
            return VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;

        case BufferUsage::AccelerationStructureStorage:
        case BufferUsage::ShaderBindingTable: return VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;

        default: break;
    }
    return VkPipelineStageFlags{};
}

bool Buffer::IsMemoryMapped() const
{
    return m_mappedMemory != nullptr;
}

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