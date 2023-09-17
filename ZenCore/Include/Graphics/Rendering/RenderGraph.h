#pragma once
#include "RenderGraphDefinitions.h"
#include "Common/UniquePtr.h"
#include <vector>
#include <vulkan/vulkan.h>
#include <unordered_set>
#include <unordered_map>

namespace zen
{

class RenderGraph;

class RDGResource
{
public:
    enum class Type
    {
        Buffer,
        Image
    };

    RDGResource(Index index, Type type, Tag tag) :
        m_index(index), m_type(type), m_name(std::move(tag)) {}

    virtual ~RDGResource() = default;

    void WriteInPass(Index index)
    {
        m_writtenInPasses.insert(index);
    }

    void ReadInPass(Index index)
    {
        m_readInPasses.insert(index);
    }

    const auto& GetReadInPasses() const { return m_readInPasses; }
    const auto& GetWrittenInPasses() const { return m_writtenInPasses; }

    void SetPhysicalIndex(Index index) { m_physicalIndex = index; }
    auto GetPhysicalIndex() const { return m_physicalIndex; }

    void ClearState()
    {
        m_readInPasses.clear();
        m_writtenInPasses.clear();
    }

    auto GetType() const { return m_type; }

private:
    Index m_index;
    Index m_physicalIndex;
    Type  m_type;
    Tag   m_name;

    std::unordered_set<Index> m_readInPasses;
    std::unordered_set<Index> m_writtenInPasses;
};

class RDGImage : public RDGResource
{
public:
    explicit RDGImage(Index index, Tag tag) :
        RDGResource(index, RDGResource::Type::Image, std::move(tag)) {}

    struct Info
    {
        VkFormat          format{VK_FORMAT_UNDEFINED};
        uint32_t          samples{1};
        uint32_t          levels{1};
        uint32_t          layer{1};
        VkImageUsageFlags usage{0};
        ImageSizeType     sizeType{ImageSizeType::SwapchainRelative};
        float             sizeX{1.0};
        float             sizeY{1.0};
        float             sizeZ{0.0};
    };

    void SetInfo(const Info& info) { m_info = info; }

    void AddImageUsage(VkImageUsageFlags flag) { m_info.usage |= flag; }

    const auto& GetInfo() { return m_info; }

private:
    Info m_info{};
};

class RDGBuffer : public RDGResource
{
public:
    explicit RDGBuffer(Index index, Tag tag) :
        RDGResource(index, RDGResource::Type::Buffer, std::move(tag)) {}

    struct Info
    {
        VkDeviceSize       size{0};
        VkBufferUsageFlags usage{0};
    };

    void SetInfo(const Info& info) { m_info = info; }

    const auto& GetInfo() { return m_info; }

    void AddBufferUsage(VkBufferUsageFlags flag) { m_info.usage |= flag; }

private:
    Info m_info{};
};

/**
 * @brief A render graph pass in a render graph
 * @note Distinguish between vulkan's render pass
 */
class RDGPass
{
public:
    RDGPass(RenderGraph& graph, Index index, Tag tag, RDGQueueFlags queueFlags);

    void WriteToColorImage(const Tag& tag, const RDGImage::Info& info);

    void ReadFromAttachment(const Tag& tag);

    void WriteToStorageBuffer(const Tag& tag, const RDGBuffer::Info& info);

    void ReadFromStorageBuffer(const Tag& tag);

    void WriteToDepthStencilImage(const Tag& tag, const RDGImage::Info& info);

    void ReadFromDepthStencilImage(const Tag& tag);

    const auto& GetIndex() const { return m_index; }
    const auto& GetOutColorImages() const { return m_outColorImages; }
    const auto& GetOutStorageImages() const { return m_outStorageImages; }
    const auto& GetOutStorageBuffers() const { return m_outStorageBuffers; }
    const auto& GetOutDepthStencil() const { return m_outDepthStencil; }
    const auto& GetInAttachments() const { return m_inAttachments; }
    const auto& GetInStorageBuffers() const { return m_inStorageBuffers; }
    const auto& GetInDepthStencil() const { return m_inDepthStencil; }

private:
    RenderGraph&  m_graph;
    Index         m_index;
    Tag           m_tag;
    RDGQueueFlags m_queueFlags;
    // output resources
    std::vector<RDGImage*>  m_outColorImages;
    std::vector<RDGImage*>  m_outStorageImages;
    std::vector<RDGBuffer*> m_outStorageBuffers;
    RDGImage*               m_outDepthStencil{nullptr};
    // input resources
    std::vector<RDGImage*>  m_inAttachments;
    std::vector<RDGBuffer*> m_inStorageBuffers;
    RDGImage*               m_inDepthStencil{nullptr};
};

class RenderGraph
{
public:
    RDGImage*  GetImageResource(const Tag& tag);
    RDGBuffer* GetBufferResource(const Tag& tag);

    RDGPass& AddPass(const Tag& tag, RDGQueueFlags queueFlags);

    void SetBackBufferTag(const Tag& tag);

    void Compile();

private:
    void SortRenderPasses();

    void TraversePassDepsRecursive(Index passIndex, uint32_t level);

    static void RemoveDuplicates(std::vector<Index>& list);

    std::unordered_map<Tag, Index>         m_resourceToIndex;
    std::unordered_map<Tag, Index>         m_passToIndex;
    std::vector<UniquePtr<RDGResource>>    m_resources;
    std::vector<UniquePtr<RDGPass>>        m_passes;
    std::vector<std::unordered_set<Index>> m_passDeps;
    std::vector<Index>                     m_sortedPassIndices;

    Tag m_backBufferTag;
};
} // namespace zen