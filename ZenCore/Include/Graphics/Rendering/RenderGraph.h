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
        m_index(index), m_type(type), m_tag(std::move(tag)) {}

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

    auto GetTag() const { return m_tag; }

private:
    Index m_index;
    Index m_physicalIndex;
    Type  m_type;
    Tag   m_tag;

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

    auto GetUsage() const { return m_info.usage; }

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

    auto GetUsage() const { return m_info.usage; }

private:
    Info m_info{};
};

struct RDGAccessedTexture
{
    RDGImage* registry{nullptr};
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

    void WriteToTransferDstBuffer(const Tag& tag, const RDGBuffer::Info& info);

    void ReadFromStorageBuffer(const Tag& tag);

    void WriteToDepthStencilImage(const Tag& tag, const RDGImage::Info& info);

    void ReadFromDepthStencilImage(const Tag& tag);

    void ReadFromGenericTexture(const Tag& tag);

    const auto& GetIndex() const { return m_index; }
    const auto& GetOutImageResources() const { return m_outImageResources; }
    const auto& GetOutBufferResources() const { return m_outBufferResources; }
    const auto& GetOutDepthStencil() const { return m_outDepthStencil; }
    const auto& GetInImageResources() const { return m_inImagesResources; }
    const auto& GetInBufferResources() const { return m_inBuffersResources; }
    const auto& GetInDepthStencil() const { return m_inDepthStencil; }
    const auto& GetTag() const { return m_tag; }

private:
    RenderGraph&  m_graph;
    Index         m_index;
    Tag           m_tag;
    RDGQueueFlags m_queueFlags;
    // output resources
    std::vector<RDGImage*>  m_outImageResources;
    std::vector<RDGBuffer*> m_outBufferResources;
    RDGImage*               m_outDepthStencil{nullptr};
    // input resources
    std::vector<RDGImage*>  m_inImagesResources;
    std::vector<RDGBuffer*> m_inBuffersResources;
    RDGImage*               m_inDepthStencil{nullptr};
    // input texture resources
    std::vector<RDGAccessedTexture> m_inTextures;
    //    std::vector<RDGImage*>  m_outColorImages;
    //    std::vector<RDGImage*>  m_outStorageImages;
    //    std::vector<RDGBuffer*> m_outStorageBuffers;
    //    std::vector<RDGBuffer*> m_outTransferDstBuffers;

    //    // input resources
    //    std::vector<RDGImage*>  m_inAttachments;
    //    std::vector<RDGBuffer*> m_inStorageBuffers;
};

struct ImageTransition
{
    VkImageUsageFlags srcUsage;
    VkImageUsageFlags dstUsage;
};

struct BufferTransition
{
    VkBufferUsageFlags srcUsage;
    VkBufferUsageFlags dstUsage;
};

struct ResourceState
{
    using ImageTransitionMap  = std::unordered_map<Tag, ImageTransition>;
    using BufferTransitionMap = std::unordered_map<Tag, BufferTransition>;
    std::unordered_map<Tag, ImageTransitionMap>  perPassImageState;
    std::unordered_map<Tag, BufferTransitionMap> perPassBufferState;
    std::unordered_map<Tag, VkFlags>             totalUsages;
    std::unordered_map<Tag, Tag>                 imageFirstUsePass;
    std::unordered_map<Tag, Tag>                 imageLastUsePass;
    std::unordered_map<Tag, Tag>                 bufferFirstUsePass;
    std::unordered_map<Tag, Tag>                 bufferLastUsePass;
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

    void ResolveResourceState();

    std::unordered_map<Tag, Index>         m_resourceToIndex;
    std::unordered_map<Tag, Index>         m_passToIndex;
    std::vector<UniquePtr<RDGResource>>    m_resources;
    std::vector<UniquePtr<RDGPass>>        m_passes;
    std::vector<std::unordered_set<Index>> m_passDeps;
    std::vector<Index>                     m_sortedPassIndices;
    // Tracking resource state
    ResourceState m_resourceState;

    Tag m_backBufferTag;
};
} // namespace zen