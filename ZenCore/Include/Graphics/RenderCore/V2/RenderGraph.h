#pragma once
#include "Common/Errors.h"
#include "Common/HashMap.h"
#include "Common/PagedAllocator.h"
#include "Common/SmallVector.h"
#include "Graphics/RHI/RHICommon.h"
#include "Graphics/RHI/RHICommands.h"
#include "RenderCoreDefs.h"

// RDG_ID class
class RDG_ID
{
public:
    static constexpr int32_t UndefinedValue = -1;
    // Constructors
    RDG_ID() : m_value(UndefinedValue) {} // Default constructor with "undefined" value
    RDG_ID(int32_t id) : m_value(id) {}   // Constructor with int32_t value

    // Copy constructor
    RDG_ID(const RDG_ID& other) : m_value(other.m_value) {}

    // Move constructor
    RDG_ID(RDG_ID&& other) noexcept : m_value(other.m_value)
    {
        other.m_value = UndefinedValue;
    }

    // Copy assignment operator
    RDG_ID& operator=(const RDG_ID& other)
    {
        if (this != &other)
        {
            m_value = other.m_value;
        }
        return *this;
    }

    // Move assignment operator
    RDG_ID& operator=(RDG_ID&& other) noexcept
    {
        if (this != &other)
        {
            m_value       = other.m_value;
            other.m_value = UndefinedValue;
        }
        return *this;
    }

    // Assignment from int32_t
    RDG_ID& operator=(int32_t id)
    {
        m_value = id;
        return *this;
    }

    // Conversion operator to int32_t
    operator int32_t() const
    {
        return m_value;
    }

    // Equality operators
    bool operator==(const RDG_ID& other) const
    {
        return m_value == other.m_value;
    }

    bool operator!=(const RDG_ID& other) const
    {
        return m_value != other.m_value;
    }

    bool IsValid() const
    {
        return m_value != UndefinedValue;
    }

private:
    int32_t m_value;
};

namespace std
{
template <> struct hash<RDG_ID>
{
    std::size_t operator()(const RDG_ID& id) const noexcept
    {
        return std::hash<int32_t>()(id);
    }
};
} // namespace std

namespace zen::rc
{
// Singleton class to generate IDs
class RDGIDGenerator
{
public:
    // Returns the singleton instance
    static RDGIDGenerator& GetInstance()
    {
        static RDGIDGenerator instance;
        return instance;
    }

    // Get the next ID
    int64_t GenID()
    {
        return m_currentID++;
    }

    // Delete copy constructor and assignment operator
    RDGIDGenerator(const RDGIDGenerator&) = delete;
    void operator=(const RDGIDGenerator&) = delete;

private:
    // Private constructor to prevent instantiation
    RDGIDGenerator() : m_currentID(0) {} // Start ID from 1
    // Current ID
    int64_t m_currentID;
};



// using RDG_ID  = int32_t;
using RDG_TAG = std::string;

enum class RDGPassType : uint32_t
{
    eCompute  = 0,
    eGraphics = 1,
    eMax      = 2
};

enum class RDGPassCmdType : uint32_t
{
    eNone                = 0,
    eBindIndexBuffer     = 1,
    eBindVertexBuffer    = 2,
    eBindPipeline        = 3,
    eClearAttachment     = 4,
    eDraw                = 5,
    eDrawIndexed         = 6,
    eDrawIndexedIndirect = 7,
    eExecuteCommands     = 8,
    eNextSubpass         = 9,
    eDispatch            = 10,
    eDispatchIndirect    = 11,
    eSetPushConstant     = 12,
    eSetLineWidth        = 13,
    eSetBlendConstant    = 14,
    eSetScissor          = 15,
    eSetViewport         = 16,
    eSetDepthBias        = 17,
    eMax                 = 18
};

enum class RDGNodeType : uint32_t
{
    eNone             = 0,
    eClearBuffer      = 1,
    eCopyBuffer       = 2,
    eUpdateBuffer     = 3,
    eClearTexture     = 4,
    eCopyTexture      = 5,
    eReadTexture      = 6,
    eUpdateTexture    = 7,
    eResolveTexture   = 8,
    eGenTextureMipmap = 9,
    eGraphicsPass     = 10,
    eComputePass      = 11,
    eMax              = 12
};

enum class RDGResourceType : uint32_t
{
    eNone    = 0,
    eBuffer  = 1,
    eTexture = 2,
    eMax     = 3
};

enum class RDGResourceUsage
{
    eNone
};

struct RDGResourceTracker
{
    rhi::AccessMode accessMode{};
    rhi::BufferUsage bufferUsage{rhi::BufferUsage::eMax};
    rhi::TextureUsage textureUsage{rhi::TextureUsage::eMax};
    rhi::TextureSubResourceRange textureSubResourceRange;
};

struct RDGAccess
{
    rhi::AccessMode accessMode{};
    RDG_ID nodeId{-1};
    RDG_ID resourceId{-1};
    rhi::BufferUsage bufferUsage{rhi::BufferUsage::eMax};
    rhi::TextureUsage textureUsage{rhi::TextureUsage::eMax};
    BitField<rhi::AccessFlagBits> accessFlags;
    rhi::TextureSubResourceRange textureSubResourceRange;
};

struct RDGResource
{
    RDG_ID id{-1};
    std::string tag; // todo: set tag in a more elegent way
    rhi::Handle physicalHandle;
    RDGResourceType type{RDGResourceType::eNone};
    RDGResourceTracker* tracker{nullptr};
    HashMap<rhi::AccessMode, std::vector<RDG_ID>> accessNodeMap;
};

struct RDGNodeBase
{
    RDG_ID id{-1};
    std::string tag;
    RDGNodeType type{RDGNodeType::eNone};
    BitField<rhi::PipelineStageBits> selfStages;
};

struct RDGPassNode;
struct RDGPassChildNode
{
    RDGPassCmdType type{RDGPassCmdType::eNone};
    RDGPassNode* parent{nullptr};
};

struct RDGPassNode : RDGNodeBase
{
    // std::vector<RDGPassChildNode*> childNodes;
    // std::vector<RDGResource*> resources;
};

struct RDGComputePassNode : RDGPassNode
{};

struct RDGGraphicsPassNode : RDGPassNode
{
    rhi::RenderPassHandle renderPass;
    rhi::FramebufferHandle framebuffer;
    rhi::Rect2<int> renderArea;
    // todo: maxColorAttachments + 1, set based on GPU limits
    uint32_t numAttachments;
    rhi::RenderPassClearValue clearValues[8];
    rhi::RenderPassLayout renderPassLayout;
    bool dynamic;
};

/*****************************/
/********* RDGNodes **********/
/*****************************/

struct RDGBufferClearNode : RDGNodeBase
{
    rhi::BufferHandle buffer;
    uint32_t offset{0};
    uint32_t size{0};
};

struct RDGBufferCopyNode : RDGNodeBase
{
    rhi::BufferHandle srcBuffer;
    rhi::BufferHandle dstBuffer;
    rhi::BufferCopyRegion region;
};

struct RDGBufferUpdateNode : RDGNodeBase
{
    std::vector<rhi::BufferCopySource> sources;
    rhi::BufferHandle dstBuffer;
};

struct RDGTextureClearNode : RDGNodeBase
{
    rhi::TextureHandle texture;
    rhi::TextureSubResourceRange range;
    rhi::Color color;
};

struct RDGTextureCopyNode : RDGNodeBase
{
    rhi::TextureHandle srcTexture;
    rhi::TextureHandle dstTexture;
    std::vector<rhi::TextureCopyRegion> copyRegions;
};

struct RDGTextureReadNode : RDGNodeBase
{
    rhi::TextureHandle srcTexture;
    rhi::BufferHandle dstBuffer;
    std::vector<rhi::BufferTextureCopyRegion> bufferTextureCopyRegions;
};

struct RDGTextureUpdateNode : RDGNodeBase
{
    std::vector<rhi::BufferTextureCopySource> sources;
    rhi::TextureHandle dstTexture;
};

struct RDGTextureResolveNode : RDGNodeBase
{
    rhi::TextureHandle srcTexture;
    rhi::TextureHandle dstTexture;
    uint32_t srcLayer{0};
    uint32_t srcMipmap{0};
    uint32_t dstLayer{0};
    uint32_t dstMipmap{0};
};

struct RDGTextureMipmapGenNode : RDGNodeBase
{
    rhi::TextureHandle texture;
};

struct RDGBindIndexBufferNode : RDGPassChildNode
{
    rhi::BufferHandle buffer;
    rhi::DataFormat format;
    uint32_t offset;
};

struct RDGBindVertexBufferNode : RDGPassChildNode
{
    std::vector<rhi::BufferHandle> vertexBuffers;
    std::vector<uint64_t> offsets;
};

struct RDGBindPipelineNode : RDGPassChildNode
{
    rhi::PipelineHandle pipeline;
    rhi::PipelineType pipelineType{rhi::PipelineType::eNone};
};

struct RDGDrawNode : RDGPassChildNode
{
    uint32_t vertexCount{0};
    uint32_t instanceCount{0};
};

struct RDGDrawIndexedNode : RDGPassChildNode
{
    uint32_t indexCount{0};
    uint32_t instanceCount{0};
    uint32_t firstIndex{0};
    int32_t vertexOffset{0};
    uint32_t firstInstance{0};
};

struct RDGDrawIndexedIndirectNode : RDGPassChildNode
{
    rhi::BufferHandle indirectBuffer;
    uint32_t offset{0};
    uint32_t drawCount{0};
    uint32_t stride{0};
};

struct RDGDispatchNode : RDGPassChildNode
{
    uint32_t groupCountX{0};
    uint32_t groupCountY{0};
    uint32_t groupCountZ{0};
};

struct RDGDispatchIndirectNode : RDGPassChildNode
{
    rhi::BufferHandle indirectBuffer;
    uint32_t offset{0};
};

struct RDGSetPushConstantsNode : RDGPassChildNode
{
    rhi::ShaderHandle shader;
    std::vector<uint8_t> data;
};

struct RDGSetBlendConstantsNode : RDGPassChildNode
{
    rhi::Color color;
};

struct RDGSetLineWidthNode : RDGPassChildNode
{
    float width{1.0f};
};

struct RDGSetScissorNode : RDGPassChildNode
{
    rhi::Rect2<int> scissor;
};

struct RDGSetViewportNode : RDGPassChildNode
{
    rhi::Rect2<float> viewport;
};

struct RDGSetDepthBiasNode : RDGPassChildNode
{
    float depthBiasConstantFactor;
    float depthBiasClamp;
    float depthBiasSlopeFactor;
};

// todo: refactor RDG AddXXXNode functions, resolve dependency when adding to graph, maintain deps by using adj list for graph datastructure
class RenderGraph
{
public:
    RenderGraph() : m_resourceAllocator(ZEN_DEFAULT_PAGESIZE, false)
    {
        m_resourceAllocator.Init();
    }

    ~RenderGraph()
    {
        Destroy();
    }

    void AddPassBindPipelineNode(RDGPassNode* parent,
                                 rhi::PipelineHandle pipelineHandle,
                                 rhi::PipelineType pipelineType);

    RDGPassNode* AddComputePassNode(const ComputePass& computePass, std::string tag);

    void AddComputePassDispatchNode(RDGPassNode* parent,
                                    uint32_t groupCountX,
                                    uint32_t groupCountY,
                                    uint32_t groupCountZ);

    void AddComputePassDispatchIndirectNode(RDGPassNode* parent,
                                            rhi::BufferHandle indirectBuffer,
                                            uint32_t offset);

    RDGPassNode* AddGraphicsPassNode(rhi::RenderPassHandle renderPassHandle,
                                     rhi::FramebufferHandle framebufferHandle,
                                     rhi::Rect2<int> area,
                                     VectorView<rhi::RenderPassClearValue> clearValues,
                                     bool hasColorTarget,
                                     bool hasDepthTarget = false);

    RDGPassNode* AddGraphicsPassNode(const rc::GraphicsPass& gfxPass,
                                     rhi::Rect2<int> area,
                                     VectorView<rhi::RenderPassClearValue> clearValues,
                                     std::string tag);

    void AddGraphicsPassBindIndexBufferNode(RDGPassNode* parent,
                                            rhi::BufferHandle bufferHandle,
                                            rhi::DataFormat format,
                                            uint32_t offset = 0);

    void AddGraphicsPassBindVertexBufferNode(RDGPassNode* parent,
                                             VectorView<rhi::BufferHandle> vertexBuffers,
                                             VectorView<uint32_t> offsets);

    void AddGraphicsPassSetPushConstants(RDGPassNode* parent, const void* data, uint32_t dataSize);

    void AddComputePassSetPushConstants(RDGPassNode* parent, const void* data, uint32_t dataSize);

    void AddGraphicsPassDrawNode(RDGPassNode* parent, uint32_t vertexCount, uint32_t instanceCount);

    void AddGraphicsPassDrawIndexedNode(RDGPassNode* parent,
                                        uint32_t indexCount,
                                        uint32_t instanceCount,
                                        uint32_t firstIndex,
                                        int32_t vertexOffset   = 0,
                                        uint32_t firstInstance = 0);

    void AddGraphicsPassDrawIndexedIndirectNode(RDGPassNode* parent,
                                                rhi::BufferHandle indirectBuffer,
                                                uint32_t offset,
                                                uint32_t drawCount,
                                                uint32_t stride);

    void AddGraphicsPassSetBlendConstantNode(RDGPassNode* parent, const rhi::Color& color);

    void AddGraphicsPassSetLineWidthNode(RDGPassNode* parent, float width);

    void AddGraphicsPassSetScissorNode(RDGPassNode* parent, const rhi::Rect2<int>& scissor);

    void AddGraphicsPassSetViewportNode(RDGPassNode* parent, const rhi::Rect2<float>& viewport);

    void AddGraphicsPassSetDepthBiasNode(RDGPassNode* parent,
                                         float depthBiasConstantFactor,
                                         float depthBiasClamp,
                                         float depthBiasSlopeFactor);

    void AddBufferClearNode(rhi::BufferHandle bufferHandle, uint32_t offset, uint64_t size);

    void AddBufferCopyNode(rhi::BufferHandle srcBufferHandle,
                           rhi::BufferHandle dstBufferHandle,
                           const rhi::BufferCopyRegion& copyRegion);

    void AddBufferUpdateNode(rhi::BufferHandle dstBufferHandle,
                             const VectorView<rhi::BufferCopySource>& sources);

    void AddTextureClearNode(rhi::TextureHandle textureHandle,
                             const rhi::Color& color,
                             const rhi::TextureSubResourceRange& range);

    void AddTextureCopyNode(rhi::TextureHandle srcTextureHandle,
                            const rhi::TextureSubResourceRange& srcTextureSubresourceRange,
                            rhi::TextureHandle dstTextureHandle,
                            const rhi::TextureSubResourceRange& dstTextureSubresourceRange,
                            const VectorView<rhi::TextureCopyRegion>& regions);

    void AddTextureReadNode(rhi::TextureHandle srcTextureHandle,
                            rhi::BufferHandle dstBufferHandle,
                            const VectorView<rhi::BufferTextureCopyRegion>& regions);

    void AddTextureUpdateNode(rhi::TextureHandle dstTextureHandle,
                              const VectorView<rhi::BufferTextureCopySource>& sources);

    void AddTextureResolveNode(rhi::TextureHandle srcTextureHandle,
                               rhi::TextureHandle dstTextureHandle,
                               uint32_t srcLayer,
                               uint32_t srcMipmap,
                               uint32_t dstLayer,
                               uint32_t dstMipMap);

    void AddTextureMipmapGenNode(rhi::TextureHandle textureHandle);

    void DeclareTextureAccessForPass(const RDGPassNode* passNode,
                                     rhi::TextureHandle textureHandle,
                                     rhi::TextureUsage usage,
                                     const rhi::TextureSubResourceRange& range,
                                     rhi::AccessMode accessMode,
                                     std::string tag = "");

    void DeclareTextureAccessForPass(const RDGPassNode* passNode,
                                     uint32_t numTextures,
                                     rhi::TextureHandle* textureHandles,
                                     rhi::TextureUsage usage,
                                     rhi::TextureSubResourceRange* ranges,
                                     rhi::AccessMode accessMode);

    void DeclareBufferAccessForPass(const RDGPassNode* passNode,
                                    rhi::BufferHandle bufferHandle,
                                    rhi::BufferUsage usage,
                                    rhi::AccessMode accessMode,
                                    std::string tag = "");

    void Begin();

    void End();

    void Execute(rhi::RHICommandList* cmdList);

private:
    void Destroy();

    static uint64_t CreateNodePairKey(const RDG_ID& nodeId1, const RDG_ID& nodeId2)
    {
        uint64_t key = static_cast<uint64_t>(static_cast<uint32_t>(nodeId1)) << 32;
        key |= static_cast<uint32_t>(nodeId2);
        return key;
    }

    void RunNode(RDGNodeBase* node);

    void SortNodes();

    bool AddNodeDepsForResource(RDGResource* resource,
                                HashMap<RDG_ID, std::vector<RDG_ID>>& nodeDepedencies,
                                const RDG_ID& srcNodeId,
                                const RDG_ID& dstNodeId);

    void SortNodesV2();

    void EmitTransitionBarriers(uint32_t level);

    void EmitInitializationBarriers(uint32_t level);

    template <class T>
        requires std::derived_from<T, RDGNodeBase>
    T* AllocNode()
    {
        uint32_t nodeDataOffset = m_nodeData.size();
        m_nodeDataOffset.push_back(nodeDataOffset);
        m_nodeData.resize(m_nodeData.size() + sizeof(T));
        T* newNode = reinterpret_cast<T*>(&m_nodeData[nodeDataOffset]);

        new (newNode) T();
        // *newNode   = T();

        newNode->id = m_nodeCount;
        m_nodeCount++;
        m_allNodes.push_back(newNode);
        return newNode;
    }

    template <class T>
        requires std::derived_from<T, RDGPassChildNode>
    T* AllocPassChildNode(RDGPassNode* passNode)
    {
        T* newNode      = new T();
        newNode->parent = passNode;
        // passNode->childNodes.push_back(newNode);
        m_passChildNodes[passNode->id].push_back(newNode);
        m_allChildNodes.push_back(newNode);
        return newNode;
    }

    RDGNodeBase* GetNodeBaseById(const RDG_ID& nodeId)
    {
        const auto dataOffset = m_nodeDataOffset[nodeId];
        return reinterpret_cast<RDGNodeBase*>(&m_nodeData[dataOffset]);
    }

    RDGNodeBase* GetNodeBaseById(int32_t idx)
    {
        const auto dataOffset = m_nodeDataOffset[idx];
        return reinterpret_cast<RDGNodeBase*>(&m_nodeData[dataOffset]);
    }

    RDGResource* GetOrAllocResource(rhi::Handle handle, RDGResourceType type, const RDG_ID& nodeId)
    {
        RDGResource* resource;
        if (!m_resourceMap.contains(handle))
        {
            resource                 = m_resourceAllocator.Alloc();
            resource->id             = static_cast<int32_t>(m_resources.size());
            resource->tracker        = CreateResourceTracker();
            resource->type           = type;
            resource->physicalHandle = handle;

            m_resources.push_back(resource);
            m_resourceMap[handle] = resource;
            // track first used node
            m_resourceFirstUseNodeMap[resource->id] = nodeId;
        }
        else
        {
            resource = m_resourceMap[handle];
        }
        return resource;
    }

    template <class T>
        requires std::derived_from<T, RDGNodeBase>
    static RDGNodeBase* ToBaseNode(T* derived)
    {
        return static_cast<RDGNodeBase*>(derived);
    }

    template <class T>
        requires std::derived_from<T, RDGNodeBase>
    static const RDGNodeBase* ToBaseNode(const T* derived)
    {
        return static_cast<RDGNodeBase*>(derived);
    }

    static RDGResourceTracker* CreateResourceTracker()
    {
        if (!s_trackerPool.empty())
        {
            RDGResourceTracker* tracker = s_trackerPool.back();
            s_trackerPool.pop_back();
            return tracker;
        }
        return new RDGResourceTracker();
    }

    static void DestroyResourceTracker(RDGResourceTracker* tracker)
    {
        s_trackerPool.push_back(tracker);
    }

    // RHI CommandList
    rhi::RHICommandList* m_cmdList{nullptr};
    // stores dependency between graph nodes
    HashMap<RDG_ID, std::vector<RDG_ID>> m_adjacencyList;
    // nodes
    std::vector<uint8_t> m_nodeData;
    std::vector<uint32_t> m_nodeDataOffset; // m_nodeDataOffset.size() = m_nodeCount
    std::vector<RDGNodeBase*> m_allNodes;
    uint32_t m_nodeCount{0};
    std::vector<std::vector<RDG_ID>> m_sortedNodes;
    std::vector<RDGPassChildNode*> m_allChildNodes;
    HashMap<RDG_ID, std::vector<RDGPassChildNode*>> m_passChildNodes;

    // tracked resources
    std::vector<RDGResource*> m_resources;
    PagedAllocator<RDGResource> m_resourceAllocator;
    HashMap<rhi::Handle, RDGResource*> m_resourceMap;
    // resource id -> node id
    HashMap<RDG_ID, RDG_ID> m_resourceFirstUseNodeMap;
    HashMap<RDG_ID, std::vector<RDGAccess>> m_nodeAccessMap;
    // transitions
    HashMap<uint64_t, std::vector<rhi::BufferTransition>> m_bufferTransitions;
    HashMap<uint64_t, std::vector<rhi::TextureTransition>> m_textureTransitions;

    static std::vector<RDGResourceTracker*> s_trackerPool;
};
} // namespace zen::rc
