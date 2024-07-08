#pragma once
#include <string>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include "DeviceObject.h"

namespace zen::val
{
enum class BufferUsage : uint32_t
{
    Undefined                      = 0,
    TransferSrc                    = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    TransferDst                    = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    UniformTexelBuffer             = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT,
    StorageTexelBuffer             = VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT,
    UniformBuffer                  = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
    StorageBuffer                  = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
    IndexBuffer                    = VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
    VertexBuffer                   = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
    IndirectBuffer                 = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
    ShaderDeviceAddress            = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    TransformFeedbackBuffer        = VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT,
    TransformFeedbackCounterBuffer = VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT,
    ConditionalRendering           = VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT,
    AccelerationStructureBuildInputReadOnly =
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
    AccelerationStructureStorage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
    ShaderBindingTable           = VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR,
    MaxEnum                      = VK_BUFFER_USAGE_FLAG_BITS_MAX_ENUM,
};

inline BufferUsage operator|(BufferUsage lhs, BufferUsage rhs)
{
    return static_cast<BufferUsage>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}
// Bitwise OR assignment (|=) operator overload as a member function
inline BufferUsage& operator|=(BufferUsage& lhs, BufferUsage rhs)
{
    lhs = static_cast<BufferUsage>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
    return lhs;
}


struct BufferCreateInfo
{
    VkDeviceSize byteSize;
    BufferUsage usage;
    VmaAllocationCreateFlags vmaFlags;
};

class Buffer : public DeviceObject<VkBuffer, VK_OBJECT_TYPE_BUFFER>
{
public:
    static SharedPtr<Buffer> Create(const Device& device, const BufferCreateInfo& CI);
    static UniquePtr<Buffer> CreateUnique(const Device& device, const BufferCreateInfo& CI);

    Buffer(const Device& device,
           VkDeviceSize byteSize,
           VkBufferUsageFlags usage,
           VmaAllocationCreateFlags vmaFlags);

    ~Buffer();

    static VkAccessFlags UsageToAccessFlags(BufferUsage usage);
    static VkPipelineStageFlags UsageToPipelineStage(BufferUsage usage);

    bool IsMemoryMapped() const;

    uint8_t* MapMemory();

    void UnmapMemory();

    void FlushMemory(size_t byteSize, size_t offset);

    void CopyData(const uint8_t* data, size_t byteSize, size_t offset);

    auto GetSize() const
    {
        return m_byteSize;
    }

private:
    VmaAllocation m_allocation{nullptr};
    VkDeviceSize m_byteSize{0};
    uint8_t* m_mappedMemory{nullptr};
};
} // namespace zen::val