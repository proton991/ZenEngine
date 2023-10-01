#pragma once
#include "Graphics/Val/Buffer.h"
#include "Common/ArrayView.h"

namespace zen
{
class StagingBuffer : public val::Buffer
{
public:
    struct SubmitInfo
    {
        size_t size;
        size_t offset;
    };

public:
    static UniquePtr<StagingBuffer> CreateUnique(val::Device& valDevice, size_t byteSize)
    {
        return MakeUnique<StagingBuffer>(valDevice, byteSize);
    }

    StagingBuffer(val::Device& valDevice, size_t byteSize) :
        val::Buffer(valDevice, byteSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT)
    {
        MapMemory();
    }

    void Flush()
    {
        FlushMemory(m_currentOffset, 0);
    }

    void Reset()
    {
        m_currentOffset = 0;
    }
    
    SubmitInfo Submit(const uint8_t* data, size_t byteSize)
    {
        ASSERT(m_currentOffset + byteSize <= GetSize());
        if (data != nullptr)
        {
            CopyData(data, byteSize, m_currentOffset);
        }
        m_currentOffset += byteSize;
        return SubmitInfo{byteSize, m_currentOffset - byteSize};
    }

    template <typename T>
    SubmitInfo Submit(ArrayView<const T> arrayView)
    {
        return Submit(static_cast<const uint8_t*>(arrayView.data()), static_cast<uint32_t>(arrayView.size() * sizeof(T)));
    }

    template <typename T>
    SubmitInfo Submit(ArrayView<T> arrayView)
    {
        return Submit(static_cast<const uint8_t*>(arrayView.data()), static_cast<uint32_t>(arrayView.size() * sizeof(T)));
    }

    template <typename T>
    SubmitInfo Submit(const T* data)
    {
        return Submit(static_cast<uint8_t*>(data), uint32_t(sizeof(T)));
    }

private:
    uint32_t m_currentOffset{0};
};

class UniformBuffer : public val::Buffer
{
public:
    static UniquePtr<UniformBuffer> CreateUnique(val::Device& valDevice, VkDeviceSize byteSize)
    {
        return MakeUnique<UniformBuffer>(valDevice, byteSize);
    }

    UniformBuffer(val::Device& valDevice, VkDeviceSize byteSize) :
        val::Buffer(valDevice, byteSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0) {}
};

class VertexBuffer : public val::Buffer
{
public:
    static UniquePtr<VertexBuffer> CreateUnique(val::Device& valDevice, VkDeviceSize byteSize)
    {
        return MakeUnique<VertexBuffer>(valDevice, byteSize);
    }

    VertexBuffer(val::Device& valDevice, VkDeviceSize byteSize) :
        val::Buffer(valDevice, byteSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0) {}
};

class IndexBuffer : public val::Buffer
{
public:
    static UniquePtr<IndexBuffer> CreateUnique(val::Device& valDevice, VkDeviceSize byteSize)
    {
        return MakeUnique<IndexBuffer>(valDevice, byteSize);
    }

    IndexBuffer(val::Device& valDevice, VkDeviceSize byteSize) :
        val::Buffer(valDevice, byteSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0) {}
};
} // namespace zen