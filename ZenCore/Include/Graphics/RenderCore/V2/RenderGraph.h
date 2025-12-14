#pragma once
#include "Utils/Errors.h"
#include "Templates/ArenaVector.h"
#include "Memory/PagedAllocator.h"
#include "Memory/PoolAllocator.h"
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
    RHIAccessMode accessMode{RHIAccessMode::eNone};
    RHIBufferUsage bufferUsage{RHIBufferUsage::eNone};
    RHITextureUsage textureUsage{RHITextureUsage::eNone};
    RHITextureSubResourceRange textureSubResourceRange;
};

class RDGResourceTrackerPool
{
public:
    RDGResourceTrackerPool();
    ~RDGResourceTrackerPool();

    RDGResourceTracker* GetTracker(const RHIResource* resource);

    void UpdateTrackerState(const RHITexture* texture,
                            RHIAccessMode accessMode,
                            RHITextureUsage usage);

    void UpdateTrackerState(const RHIBuffer* buffer,
                            RHIAccessMode accessMode,
                            RHIBufferUsage usage);

private:
    // HashMap<Handle, RDGResourceTracker*> m_trackerMap;
    HashMap<const RHIResource*, RDGResourceTracker*> m_trackerMap;
    // HashMap<const TextureHandle, RDGResourceTracker*> m_trackerMap2;
    // std::vector<RDGResourceTracker*> m_trackers;
};

struct RDGAccess
{
    RHIAccessMode accessMode{};
    RDG_ID nodeId{-1};
    RDG_ID resourceId{-1};
    RHIBufferUsage bufferUsage{RHIBufferUsage::eMax};
    RHITextureUsage textureUsage{RHITextureUsage::eMax};
    BitField<RHIAccessFlagBits> accessFlags;
    RHITextureSubResourceRange textureSubResourceRange;
};

struct RDGResource
{
    RDGResource(PoolAllocator<LinearAllocator>* alloc) :
        readByNodeIds(alloc), writtenByNodeIds(alloc)
    {}

    RDG_ID id{-1};
    std::string tag;
    RHIResource* physicalRes{nullptr};
    RDGResourceType type{RDGResourceType::eNone};

    ArenaVector<RDG_ID, PoolAllocator<LinearAllocator>> readByNodeIds;
    ArenaVector<RDG_ID, PoolAllocator<LinearAllocator>> writtenByNodeIds;
};

struct RDGNodeBase
{
    RDG_ID id{-1};
    std::string tag;
    RDGNodeType type{RDGNodeType::eNone};
    BitField<RHIPipelineStageBits> selfStages;
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
{
    const ComputePass* computePass{nullptr};
};

struct RDGGraphicsPassNode : RDGPassNode
{
    RenderPassHandle renderPass;
    FramebufferHandle framebuffer;
    Rect2<int> renderArea;
    // todo: maxColorAttachments + 1, set based on GPU limits
    uint32_t numAttachments;
    RHIRenderPassClearValue clearValues[8];
    RHIRenderPassLayout renderPassLayout;
    bool dynamic;
    const GraphicsPass* graphicsPass{nullptr};
};

/*****************************/
/********* RDGNodes **********/
/*****************************/

struct RDGBufferClearNode : RDGNodeBase
{
    RHIBuffer* buffer;
    uint32_t offset{0};
    uint32_t size{0};
};

struct RDGBufferCopyNode : RDGNodeBase
{
    RHIBuffer* srcBuffer;
    RHIBuffer* dstBuffer;
    RHIBufferCopyRegion region;
};

struct RDGBufferUpdateNode : RDGNodeBase
{
    std::vector<RHIBufferCopySource> sources;
    RHIBuffer* dstBuffer;
};

struct RDGTextureClearNode : RDGNodeBase
{
    // TextureHandle texture;
    // RHITextureSubResourceRange range;
    RHITexture* texture;
    Color color;
};

struct RDGTextureCopyNode : RDGNodeBase
{
    // TextureHandle srcTexture;
    // TextureHandle dstTexture;
    uint32_t numCopyRegions;
    RHITexture* srcTexture;
    RHITexture* dstTexture;
    // RHITextureCopyRegion* copyRegions;

    RHITextureCopyRegion* TextureCopyRegions()
    {
        return reinterpret_cast<RHITextureCopyRegion*>(&this[1]);
    }

    const RHITextureCopyRegion* TextureCopyRegions() const
    {
        return reinterpret_cast<const RHITextureCopyRegion*>(&this[1]);
    }
    // std::vector<RHITextureCopyRegion> copyRegions;
};

struct RDGTextureReadNode : RDGNodeBase
{
    // TextureHandle srcTexture;
    uint32_t numCopyRegions;
    RHITexture* srcTexture;
    RHIBuffer* dstBuffer;
    // std::vector<RHIBufferTextureCopyRegion> bufferTextureCopyRegions;

    RHIBufferTextureCopyRegion* BufferTextureCopyRegions()
    {
        return reinterpret_cast<RHIBufferTextureCopyRegion*>(&this[1]);
    }

    const RHIBufferTextureCopyRegion* BufferTextureCopyRegions() const
    {
        return reinterpret_cast<const RHIBufferTextureCopyRegion*>(&this[1]);
    }
};

struct RDGTextureUpdateNode : RDGNodeBase
{
    // std::vector<RHIBufferTextureCopySource> sources;
    // TextureHandle dstTexture;
    uint32_t numCopySources;
    RHITexture* dstTexture;

    RHIBufferTextureCopySource* TextureCopySources()
    {
        return reinterpret_cast<RHIBufferTextureCopySource*>(&this[1]);
    }

    const RHIBufferTextureCopySource* TextureCopySources() const
    {
        return reinterpret_cast<const RHIBufferTextureCopySource*>(&this[1]);
    }
};

struct RDGTextureResolveNode : RDGNodeBase
{
    // TextureHandle srcTexture;
    // TextureHandle dstTexture;
    RHITexture* srcTexture;
    RHITexture* dstTexture;
    uint32_t srcLayer{0};
    uint32_t srcMipmap{0};
    uint32_t dstLayer{0};
    uint32_t dstMipmap{0};
};

struct RDGTextureMipmapGenNode : RDGNodeBase
{
    // TextureHandle texture;
    RHITexture* texture;
};

struct RDGBindIndexBufferNode : RDGPassChildNode
{
    RHIBuffer* buffer;
    DataFormat format;
    uint32_t offset;
};

// struct RDGBindVertexBufferNode : RDGPassChildNode
// {
//     std::vector<RHIBuffer*> vertexBuffers;
//     std::vector<uint64_t> offsets;
// };

struct RDGBindVertexBufferNode : RDGPassChildNode
{
    uint32_t numBuffers{0};
    // RHIBuffer** vertexBuffers{nullptr};
    // uint64_t* offsets{nullptr};
    // std::vector<RHIBuffer*> vertexBuffers;
    // std::vector<uint64_t> offsets;

    RHIBuffer** VertexBuffers()
    {
        return reinterpret_cast<RHIBuffer**>(&this[1]);
    }

    RHIBuffer* const* VertexBuffers() const
    {
        return reinterpret_cast<RHIBuffer* const*>(&this[1]);
    }

    uint64_t* VertexBufferOffsets()
    {
        return reinterpret_cast<uint64_t*>(&VertexBuffers()[numBuffers]);
    }

    const uint64_t* VertexBufferOffsets() const
    {
        return reinterpret_cast<const uint64_t*>(&VertexBuffers()[numBuffers]);
    }

    // uint64_t* VertexBufferOffsets()
    // {
    //     return reinterpret_cast<uint64_t*>(&VertexBuffers()[numBuffers]);
    // }
    //
    // const uint64_t* VertexBufferOffsets() const
    // {
    //     return reinterpret_cast<const uint64_t*>(&VertexBuffers()[numBuffers]);
    // }
};

struct RDGBindPipelineNode : RDGPassChildNode
{
    RHIPipeline* pipeline;
    RHIPipelineType pipelineType{RHIPipelineType::eNone};
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
    RHIBuffer* indirectBuffer;
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
    RHIBuffer* indirectBuffer;
    uint32_t offset{0};
};

struct RDGSetPushConstantsNode : RDGPassChildNode
{
    RHIShader* shader;
    uint32_t dataSize{0};

    uint8_t* Data()
    {
        return reinterpret_cast<uint8_t*>(&this[1]);
    }

    const uint8_t* Data() const
    {
        return reinterpret_cast<const uint8_t*>(&this[1]);
    }


    // std::vector<uint8_t> data;
};

struct RDGSetBlendConstantsNode : RDGPassChildNode
{
    Color color;
};

struct RDGSetLineWidthNode : RDGPassChildNode
{
    float width{1.0f};
};

struct RDGSetScissorNode : RDGPassChildNode
{
    Rect2<int> scissor;
};

struct RDGSetViewportNode : RDGPassChildNode
{
    Rect2<float> viewport;
};

struct RDGSetDepthBiasNode : RDGPassChildNode
{
    float depthBiasConstantFactor;
    float depthBiasClamp;
    float depthBiasSlopeFactor;
};

// todo: unify tags/names with RHIResource
class RenderGraph
{
public:
    RenderGraph(std::string tag) :
        m_rdgTag(std::move(tag)),
        m_resourceAllocator(ZEN_DEFAULT_PAGESIZE, false),
        m_poolAlloc(8 * 1024) // 8KB initial size
    {
        m_resourceAllocator.Init();
    }

    ~RenderGraph()
    {
        Destroy();
    }

    void AddPassBindPipelineNode(RDGPassNode* parent,
                                 RHIPipeline* pipelineHandle,
                                 RHIPipelineType pipelineType);

    RDGPassNode* AddComputePassNode(const ComputePass* pComputePass, std::string tag);

    void AddComputePassDispatchNode(RDGPassNode* parent,
                                    uint32_t groupCountX,
                                    uint32_t groupCountY,
                                    uint32_t groupCountZ);

    void AddComputePassDispatchIndirectNode(RDGPassNode* parent,
                                            RHIBuffer* indirectBuffer,
                                            uint32_t offset);

    RDGPassNode* AddGraphicsPassNode(RenderPassHandle renderPassHandle,
                                     FramebufferHandle framebufferHandle,
                                     Rect2<int> area,
                                     VectorView<RHIRenderPassClearValue> clearValues,
                                     bool hasColorTarget,
                                     bool hasDepthTarget = false);

    RDGPassNode* AddGraphicsPassNode(const GraphicsPass* gfxPass,
                                     Rect2<int> area,
                                     VectorView<RHIRenderPassClearValue> clearValues,
                                     std::string tag);

    void AddGraphicsPassBindIndexBufferNode(RDGPassNode* parent,
                                            RHIBuffer* bufferHandle,
                                            DataFormat format,
                                            uint32_t offset = 0);

    void AddGraphicsPassBindVertexBufferNode(RDGPassNode* parent,
                                             VectorView<RHIBuffer*> vertexBuffers,
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
                                                RHIBuffer* indirectBuffer,
                                                uint32_t offset,
                                                uint32_t drawCount,
                                                uint32_t stride);

    void AddGraphicsPassSetBlendConstantNode(RDGPassNode* parent, const Color& color);

    void AddGraphicsPassSetLineWidthNode(RDGPassNode* parent, float width);

    void AddGraphicsPassSetScissorNode(RDGPassNode* parent, const Rect2<int>& scissor);

    void AddGraphicsPassSetViewportNode(RDGPassNode* parent, const Rect2<float>& viewport);

    void AddGraphicsPassSetDepthBiasNode(RDGPassNode* parent,
                                         float depthBiasConstantFactor,
                                         float depthBiasClamp,
                                         float depthBiasSlopeFactor);

    void AddBufferClearNode(RHIBuffer* bufferHandle, uint32_t offset, uint64_t size);

    void AddBufferCopyNode(RHIBuffer* srcBufferHandle,
                           RHIBuffer* dstBufferHandle,
                           const RHIBufferCopyRegion& copyRegion);

    void AddBufferUpdateNode(RHIBuffer* dstBufferHandle,
                             const VectorView<RHIBufferCopySource>& sources);

    void AddTextureClearNode(RHITexture* texture,
                             const Color& color,
                             const RHITextureSubResourceRange& range);

    void AddTextureCopyNode(RHITexture* srcTexture,
                            RHITexture* dstTexture,
                            const VectorView<RHITextureCopyRegion>& regions);

    void AddTextureReadNode(RHITexture* srcTexture,
                            RHIBuffer* dstBufferHandle,
                            const VectorView<RHIBufferTextureCopyRegion>& regions);

    void AddTextureUpdateNode(RHITexture* dstTexture,
                              const VectorView<RHIBufferTextureCopySource>& sources);

    void AddTextureResolveNode(RHITexture* srcTexture,
                               RHITexture* dstTexture,
                               uint32_t srcLayer,
                               uint32_t srcMipmap,
                               uint32_t dstLayer,
                               uint32_t dstMipMap);

    void AddTextureMipmapGenNode(RHITexture* texture);


    void Begin();

    void End();

    void Execute(RHICommandList* cmdList);

private:
    void DeclareTextureAccessForPass(const RDGPassNode* passNode,
                                     RHITexture* texture,
                                     RHITextureUsage usage,
                                     const RHITextureSubResourceRange& range,
                                     RHIAccessMode accessMode);

    void DeclareBufferAccessForPass(const RDGPassNode* passNode,
                                    RHIBuffer* buffer,
                                    RHIBufferUsage usage,
                                    RHIAccessMode accessMode);

    void Destroy();

    static uint64_t CreateNodePairKey(const RDG_ID& nodeId1, const RDG_ID& nodeId2)
    {
        uint64_t key = static_cast<uint64_t>(static_cast<uint32_t>(nodeId1)) << 32;
        key |= static_cast<uint32_t>(nodeId2);
        return key;
    }

    void RunNode(RDGNodeBase* node);

    // void SortNodes();

    bool AddNodeDepsForResource(RDGResource* resource,
                                HashMap<RDG_ID, std::vector<RDG_ID>>& nodeDependencies,
                                const RDG_ID& srcNodeId,
                                const RDG_ID& dstNodeId);

    void SortNodesV2();

    void EmitTransitionBarriers(uint32_t level);

    void EmitInitializationBarriers(uint32_t level);

    template <class T>
        requires std::derived_from<T, RDGNodeBase>
    T* AllocNode()
    {
        // uint32_t nodeDataOffset = m_nodeData.size();
        // m_nodeDataOffset.push_back(nodeDataOffset);
        // m_nodeData.resize(m_nodeData.size() + sizeof(T));
        // T* newNode = reinterpret_cast<T*>(&m_nodeData[nodeDataOffset]);
        T* newNode = static_cast<T*>(m_poolAlloc.Alloc(sizeof(T)));

        new (newNode) T();
        // *newNode   = T();

        newNode->id = m_nodeCount;
        m_nodeCount++;
        m_baseNodeMap[newNode->id] = newNode;
        // m_allNodes.push_back(newNode);
        return newNode;
    }

    template <class T>
        requires std::derived_from<T, RDGNodeBase>
    T* AllocNode(size_t nodeSize)
    {
        // uint32_t nodeDataOffset = m_nodeData.size();
        // m_nodeDataOffset.push_back(nodeDataOffset);
        // m_nodeData.resize(m_nodeData.size() + nodeSize);
        // T* newNode = reinterpret_cast<T*>(&m_nodeData[nodeDataOffset]);
        T* newNode = static_cast<T*>(m_poolAlloc.Alloc(nodeSize));
        new (newNode) T();
        // *newNode   = T();

        newNode->id = m_nodeCount;
        m_nodeCount++;
        m_baseNodeMap[newNode->id] = newNode;
        // m_allNodes.push_back(newNode);
        return newNode;
    }

    template <class T>
        requires std::derived_from<T, RDGPassChildNode>
    T* AllocPassChildNode(RDGPassNode* passNode)
    {
        // T* newNode      = new T();
        // T* newNode      = static_cast<T*>(ZEN_MEM_ALLOC(sizeof(T)));
        T* newNode      = static_cast<T*>(m_poolAlloc.Alloc(sizeof(T)));
        newNode->parent = passNode;
        // passNode->childNodes.push_back(newNode);
        m_passChildNodeMap[passNode->id].push_back(newNode);
        // m_allChildNodes.push_back(newNode);
        return newNode;
    }

    template <class T>
        requires std::derived_from<T, RDGPassChildNode>
    T* AllocPassChildNode(RDGPassNode* passNode, size_t nodeSize)
    {
        // T* newNode = static_cast<T*>(ZEN_MEM_ALLOC(nodeSize));
        T* newNode = static_cast<T*>(m_poolAlloc.Alloc(nodeSize));
        // new (newNode) T;

        // T* newNode      = new T();
        newNode->parent = passNode;
        // uint32_t nodeDataOffset = m_nodeData.size();
        // m_nodeDataOffset.push_back(nodeDataOffset);
        // m_nodeData.resize(m_nodeData.size() + nodeSize);
        // T* newNode = reinterpret_cast<T*>(&m_nodeData[nodeDataOffset]);

        // new (newNode) T();
        // passNode->childNodes.push_back(newNode);
        m_passChildNodeMap[passNode->id].push_back(newNode);
        // m_allChildNodes.push_back(newNode);
        return newNode;
    }

    const RDGNodeBase* GetNodeBaseById(const RDG_ID& nodeId) const
    {
        return m_baseNodeMap.at(nodeId);
        // const auto dataOffset = m_nodeDataOffset[nodeId];
        // return reinterpret_cast<const RDGNodeBase*>(&m_nodeData[dataOffset]);
    }

    RDGNodeBase* GetNodeBaseById(const RDG_ID& nodeId)
    {
        return m_baseNodeMap.at(nodeId);
        // const auto dataOffset = m_nodeDataOffset[nodeId];
        // return reinterpret_cast<RDGNodeBase*>(&m_nodeData[dataOffset]);
    }

    // RDGNodeBase* GetNodeBaseById(int32_t idx)
    // {
    //     const auto dataOffset = m_nodeDataOffset[idx];
    //     return reinterpret_cast<RDGNodeBase*>(&m_nodeData[dataOffset]);
    // }

    RDGResource* GetOrAllocResource(RHIResource* resourceRHI,
                                    RDGResourceType type,
                                    const RDG_ID& nodeId)
    {
        RDGResource* resource;
        if (!m_resourceMap.contains(resourceRHI))
        {
            resource              = m_resourceAllocator.Alloc(&m_poolAlloc);
            resource->id          = static_cast<int32_t>(m_resources.size());
            resource->type        = type;
            resource->physicalRes = resourceRHI;

            m_resources.push_back(resource);
            m_resourceMap[resourceRHI] = resource;
            // track first used node
            m_resourceFirstUseNodeMap[resource->id] = nodeId;
        }
        else
        {
            resource = m_resourceMap[resourceRHI];
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

    std::string m_rdgTag;
    // RHI CommandList
    RHICommandList* m_cmdList{nullptr};
    // stores dependency between graph nodes
    HashMap<RDG_ID, std::vector<RDG_ID>> m_adjacencyList;
    // nodes
    // std::vector<uint8_t> m_nodeData;
    // std::vector<uint32_t> m_nodeDataOffset; // m_nodeDataOffset.size() = m_nodeCount
    // std::vector<RDGNodeBase*> m_allNodes;
    uint32_t m_nodeCount{0};
    std::vector<std::vector<RDG_ID>> m_sortedNodes;
    // std::vector<RDGPassChildNode*> m_allChildNodes;

    HashMap<RDG_ID, RDGNodeBase*> m_baseNodeMap;
    HashMap<RDG_ID, std::vector<RDGPassChildNode*>> m_passChildNodeMap;

    // tracked resources
    std::vector<RDGResource*> m_resources;
    PagedAllocator<RDGResource> m_resourceAllocator;

    PoolAllocator<LinearAllocator> m_poolAlloc;

    HashMap<RHIResource*, RDGResource*> m_resourceMap;
    // resource id -> node id
    HashMap<RDG_ID, RDG_ID> m_resourceFirstUseNodeMap;
    HashMap<RDG_ID, std::vector<RDGAccess>> m_nodeAccessMap;
    // transitions
    HashMap<uint64_t, std::vector<RHIBufferTransition>> m_bufferTransitions;
    HashMap<uint64_t, std::vector<RHITextureTransition>> m_textureTransitions;
    // track resource state across multiple RDG instances
    static RDGResourceTrackerPool s_trackerPool;
};
} // namespace zen::rc
