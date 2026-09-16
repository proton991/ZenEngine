#pragma once
#include "Memory/LinearAllocator.h"
#include "Utils/RefCountPtr.h"
#include "Memory/PoolAllocator.h"
#include "Templates/FlatHashMap.h"
#include "Templates/NameID.h"
#include "Graphics/RHI/RHICommon.h"
#include "Graphics/RenderCore/V2/RenderGraph/RDGDefs.h"
#include "Graphics/RenderCore/V2/RenderCoreDefs.h"

namespace zen::rc
{
class RenderDevice;
class RenderGraph;

enum class RDGResourceType : uint8_t
{
    eNone    = 0,
    eBuffer  = 1,
    eTexture = 2,
    eMax     = 3
};

enum class RDGResourceLifeCycle : uint8_t
{
    eImported,
    eTransient,
    ePersistent
};

struct RDGTextureDesc
{
    NameID name;
    TextureFormat texFormat{};
    BitField<RHITextureUsageFlagBits> usageFlags{0};
};

struct RDGBufferDesc
{
    NameID name;
    uint64_t size{0};
    BitField<RHIBufferUsageFlagBits> usageFlags{0};
};

// Payload estimates, excluding native allocation alignment/metadata. The budget limits idle
// cache ownership, not live graph resources, extracted owners, or pending GPU retirement.
struct RDGPoolConfig
{
    uint64_t budgetBytes{256ull * 1024 * 1024};
    uint64_t maxIdleBuilds{120};
};
struct RDGPoolStats
{
    uint64_t allocatedBytes{0}; // Unique assigned + available pool allocations.
    uint64_t assignedBytes{0};
    uint64_t availableBytes{0};
    uint64_t inFlightBytes{0}; // Conservative, based on submitted queue serials.
    uint64_t retiringBytes{0}; // Evicted bytes still awaiting the captured queue serials.
    uint64_t hits{0};
    uint64_t misses{0};
    uint64_t evictions{0};
    uint32_t assignedCount{0};
    uint32_t availableCount{0};
    uint32_t descriptorCount{0};
};
struct RDGPoolBucketStats
{
    RDGResourceType type{RDGResourceType::eNone};
    TextureFormat textureFormat{};
    uint64_t bufferSize{0};
    int64_t usageFlags{0};
    uint32_t availableCount{0};
    uint64_t availableBytes{0};
};

struct RDGTextureImportState
{
    RDGImportContents contents{RDGImportContents::eUnknown};
    RHIAccessMode accessMode{RHIAccessMode::eNone};
    RHITextureUsage usage{RHITextureUsage::eNone};
    BitField<RHIPipelineStageFlagBits> stages{RHIPipelineStageFlagBits::eTopOfPipe};
};
struct RDGBufferImportState
{
    RDGImportContents contents{RDGImportContents::eUnknown};
    RHIAccessMode accessMode{RHIAccessMode::eNone};
    BitField<RHIBufferUsageFlagBits> usage;
    BitField<RHIPipelineStageFlagBits> stages{RHIPipelineStageFlagBits::eTopOfPipe};
};

// A copied description of the selected resource; never exposes manager-owned storage.
struct RDGResourceInfo
{
    NameID name;
    RDGResourceType type{RDGResourceType::eNone};
    RDGResourceLifeCycle lifeCycle{RDGResourceLifeCycle::eTransient};
    int32_t version{-1}; // Transitional automatic selection; otherwise the explicit version number.
    uint64_t physicalStableId{
        0}; // Zero until materialized; diagnostic identity, not a native pointer.
};

// The shared request keeps pending publication safe if the caller moves or discards its ticket.
// Only one move-only public owner is issued; the graph separately retains its execution reference.
struct RDGExtractionState : RefCounted
{
    ~RDGExtractionState() override;

    RHIResource* resource{nullptr};
    RenderDevice* device{nullptr};
};

struct RDGDeferredExtraction
{
    RefCountPtr<RDGExtractionState> state;
    RHIResourcePtr<RHIResource> resource;
};
template <typename Resource> class RDGExtractedResource
{
public:
    RDGExtractedResource() = default;

    RDGExtractedResource(RDGExtractedResource&&) noexcept = default;

    RDGExtractedResource& operator=(RDGExtractedResource&&) noexcept = default;

    RDGExtractedResource(const RDGExtractedResource&) = delete;

    RDGExtractedResource& operator=(const RDGExtractedResource&) = delete;

    Resource* Get() const
    {
        return m_state ? static_cast<Resource*>(m_state->resource) : nullptr;
    }

    explicit operator bool() const
    {
        return Get() != nullptr;
    }

    void Reset()
    {
        m_state.Reset();
    }

private:
    friend class RDGResourceManager;

    explicit RDGExtractedResource(RefCountPtr<RDGExtractionState> state) : m_state(std::move(state))
    {}

    RefCountPtr<RDGExtractionState> m_state;
};
using RDGExtractedTexture = RDGExtractedResource<RHITexture>;
using RDGExtractedBuffer  = RDGExtractedResource<RHIBuffer>;

// todo: do not hold RenderDevice
class RDGResourceManager
{
public:
    RDGResourceManager();

    RDGResourceManager(const RDGResourceManager&) = delete;

    RDGResourceManager& operator=(const RDGResourceManager&) = delete;

    ~RDGResourceManager()
    {
        Reset();
    }

    // Wire the shared FrameArena allocator from RDG
    void SetFrameArena(PoolAllocator<LinearAllocator>* pFrameArena)
    {
        m_pFrameArena = pFrameArena;
    }

    void SetOwner(RenderGraph* graph)
    {
        m_owner = graph;
    }

    // create pooled transient resources
    RDGTexture CreateTexture(const RDGTextureDesc& desc);

    RDGBuffer CreateBuffer(const RDGBufferDesc& desc);

    // Every compiled access selects a version. Base resource values/raw bindings use automatic versions:
    // End selects the current value in pass declaration order and advances it for each writing pass.
    // Explicit versions allow producers and consumers to be declared in any order. InitialVersion
    // is read-only imported/undefined contents; CreateVersion declares one new value with exactly
    // one producing pass. It accepts a base resource value for the first version, then the preceding version.
    // Branching and mixing automatic/explicit declarations on one allocation are errors.
    RDGTexture InitialVersion(RDGTexture texture);

    RDGBuffer InitialVersion(RDGBuffer buffer);

    RDGTexture CreateVersion(RDGTexture previous);

    RDGBuffer CreateVersion(RDGBuffer previous);

    // imported resources
    RDGTexture ImportTexture(RHITexture* pTexture,
                             RDGImportContents contents = RDGImportContents::ePreserve);

    RDGBuffer ImportBuffer(RHIBuffer* pBuffer,
                           RDGImportContents contents = RDGImportContents::ePreserve);

    // External work: the caller supplies the actual state before every execution and arranges
    // queue completion/visibility before submission. These assertions do not insert queue waits.
    RDGTexture ImportTexture(RHITexture* texture, const RDGTextureImportState& state);

    RDGBuffer ImportBuffer(RHIBuffer* buffer, const RDGBufferImportState& state);

    // All ranges read by this graph must have been written by the host and made available
    // before submission (coherent memory or an explicit flush). The caller must prevent
    // overlap with in-flight GPU accesses. This does not replace tracked GPU access state.
    RDGBuffer ImportHostWrittenBuffer(RHIBuffer* pBuffer);

    // Publication occurs only after successful recording. Keep the device alive until owners die.
    // Final read usage is emitted as a terminal graph access; contents must be completely defined.
    RDGExtractedTexture QueueTextureExtraction(
        RDGTexture texture,
        RHITextureUsage finalUsage = RHITextureUsage::eSampled);

    RDGExtractedBuffer QueueBufferExtraction(
        RDGBuffer buffer,
        BitField<RHIBufferUsageFlagBits> finalUsage =
            BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eStorageBuffer));

    // Queries validate against this live manager and copy metadata. Values survive calls/replay
    // within one recorded build, never Reset/Begin. Failed queries leave the output unchanged.
    bool GetResourceInfo(RDGResource resource, RDGResourceInfo& info);

    bool GetTextureDesc(RDGTexture texture, RDGTextureDesc& desc);

    bool GetBufferDesc(RDGBuffer buffer, RDGBufferDesc& desc);

    bool IsValid(RDGResource resource);

    bool IsValid(RDGTexture texture, const RDGTextureViewDesc& view);

    // allocate physical RHIResource for transient RDGResources
    RDGResult MaterializeTransientResources(RenderDevice* pDevice);

    void ReleaseTransientResources();

    bool SetPoolConfig(const RDGPoolConfig& config);

    RDGPoolStats GetPoolStats() const;

    HeapVector<RDGPoolBucketStats> GetPoolBuckets() const;

    // allAvailable also drops obsolete extent/descriptor families after resize.
    bool TrimPool(bool allAvailable = false);

    void Destroy(RenderDevice* pDevice);

private:
    friend class RenderGraph;
    friend class RDGExecutor;
    friend class RenderDevice;
    friend class RDGMetrics;
    friend class RDGPassCompiler;
    friend class RDGTransferPassCmdRecorder;
    struct Allocation
    {
        RDG_ID id{-1};
        NameID name;
        RDGResourceType type{RDGResourceType::eNone};
        uint32_t accessCount{0};
        uint32_t liveAccessCount{0};
        uint32_t firstUse{UINT32_MAX};
        uint32_t lastUse{0};
        bool ownsPhysical{false}; // One graph reference per unique physical allocation.
        int32_t initialVersion{-1};
        bool imported{false};
        bool exported{false};
        bool hasInitialState{false};
        RDGTextureImportState initialTextureState;
        RDGBufferImportState initialBufferState;
        RDGImportContents initialContents{RDGImportContents::ePreserve};
        bool hostWritten{false}; // Explicit buffer initialization contract for this graph.
        TextureFormat texFormat{};
        uint64_t bufferSize{0};
        BitField<uint32_t> usageFlags{0u};

        RHITexture* pTexture{nullptr};
        RHIBuffer* pBuffer{nullptr};

        RDGResourceLifeCycle GetLifeCycle() const
        {
            RDGResourceLifeCycle result{};

            if (imported)
            {
                result = RDGResourceLifeCycle::eImported;
            }
            else if (exported)
            {
                result = RDGResourceLifeCycle::ePersistent;
            }
            else
            {
                result = RDGResourceLifeCycle::eTransient;
            }

            return result;
        }
    };

    const Allocation* Resolve(RDGResource resource);

    const Allocation* ResolveView(RDGResource texture, const RDGTextureViewDesc& view);

    static RHITextureSubResourceRange ViewRange(const Allocation& texture,
                                                const RDGTextureViewDesc& view);

    const Allocation* FindResourceByIdx(int32_t id) const;

    const Allocation* FindResourceByStableId(uint64_t stableId) const;

    RHITextureView* MaterializeView(RDGResource texture, const RDGTextureViewDesc& view);

    struct Extraction
    {
        Allocation* resource;
        RDGResource value;
        RHITextureUsage textureUsage{RHITextureUsage::eNone};
        BitField<RHIBufferUsageFlagBits> bufferUsage;
        RefCountPtr<RDGExtractionState> state;
    };
    HeapVector<Extraction> m_extractions;
    struct ResourceVersion
    {
        RDG_ID resourceId{-1};
        int32_t previous{-1};
        int32_t next{-1};
        uint32_t number{0};
    };
    HeapVector<ResourceVersion> m_versions;

    RDGResource InitialResourceVersion(RDGResource value);

    RDGResource CreateResourceVersion(RDGResource previous);

    struct CachedView
    {
        uint64_t textureId;
        RHITextureSubResourceRange range;
        RHITextureView* view;
    };
    HeapVector<CachedView> m_viewCache;

    RefCountPtr<RDGExtractionState> QueueExtraction(RDGResource resource,
                                                    RDGResourceType type,
                                                    RHITextureUsage textureUsage,
                                                    BitField<RHIBufferUsageFlagBits> bufferUsage);

    bool DeclareExtractions();

    void PublishExtractions(RenderDevice* device);

    void StageExtractions(HeapVector<RDGDeferredExtraction>& output);

    bool SetImportContents(const Allocation* resource, RDGImportContents contents);

    RDGResource MakeResource(const Allocation* resource) const;

    const Allocation* CreateTextureAllocation(const RDGTextureDesc& desc);

    const Allocation* CreateBufferAllocation(const RDGBufferDesc& desc);

    const Allocation* ImportTextureAllocation(RHITexture* texture);

    const Allocation* ImportBufferAllocation(RHIBuffer* buffer);

    void Retain(RHIResource* resource);

    void ReleaseRetainedResources(RenderDevice* device);

    uint64_t m_identity;
    uint64_t m_generation{1};
    FlatHashMap<uint64_t, RHIResource*> m_retainedResources;
    struct RDGTexturePoolKey
    {
        TextureFormat texFormat{};
        int64_t usageFlags{0};

        bool operator==(const RDGTexturePoolKey& other) const
        {
            return texFormat.format == other.texFormat.format &&
                texFormat.sampleCount == other.texFormat.sampleCount &&
                texFormat.dimension == other.texFormat.dimension &&
                texFormat.mutableFormat == other.texFormat.mutableFormat &&
                texFormat.width == other.texFormat.width &&
                texFormat.height == other.texFormat.height &&
                texFormat.depth == other.texFormat.depth &&
                texFormat.arrayLayers == other.texFormat.arrayLayers &&
                texFormat.mipmaps == other.texFormat.mipmaps && usageFlags == other.usageFlags;
        }
    };

    struct RDGBufferPoolKey
    {
        uint64_t size{0};
        int64_t usageFlags{0};

        bool operator==(const RDGBufferPoolKey& other) const
        {
            return size == other.size && usageFlags == other.usageFlags;
        }
    };

    struct RDGTexturePoolKeyHasher
    {
        size_t operator()(const RDGTexturePoolKey& key) const;
    };

    struct RDGBufferPoolKeyHasher
    {
        size_t operator()(const RDGBufferPoolKey& key) const;
    };

    bool CheckMutation();

    void Reject(const std::string& message, RDGErrorCode code = RDGErrorCode::eBinding);

    RenderGraph* m_owner{nullptr};

    Allocation* AllocAllocation();

    void CreatePhysicalResource(Allocation* pResource);

    // Acquire pooled RHIResource from pool, return null on miss
    bool TryAcquirePooledTexture(Allocation* pResource);

    bool TryAcquirePooledBuffer(Allocation* pResource);

    // validate helper functions
    static bool ValidateRDGTextureDesc(const RDGTextureDesc& desc);

    static bool ValidateRDGBufferDesc(const RDGBufferDesc& desc);

    static RDGTexturePoolKey MakeTexturePoolKey(const Allocation& resource);

    static RDGBufferPoolKey MakeBufferPoolKey(const Allocation& resource);

    struct PoolEntry
    {
        RHIResource* resource{nullptr};
        uint64_t bytes{0};
        uint64_t lastUsedBuild{0};
        uint64_t graphicsSerial{0};
        uint64_t transferSerial{0};
    };
    struct RetiredPoolBytes
    {
        uint64_t bytes, graphicsSerial, transferSerial;
    };

    static uint64_t EstimateBytes(const Allocation& resource);

    static bool InFlight(uint64_t graphics, uint64_t transfer);

    void RetirePoolEntry(const PoolEntry& entry);

    RDGPoolConfig m_poolConfig;
    uint64_t m_poolHits{0}, m_poolMisses{0}, m_poolEvictions{0};

    template <typename Pool> static void CountPoolStats(const Pool& pool, RDGPoolStats& stats);

    template <typename Pool>
    static void CollectPoolEntries(Pool& pool, HeapVector<PoolEntry*>& candidates);

    template <typename Pool> static void CompactPool(Pool& pool);

    HeapVector<RetiredPoolBytes> m_retiredPoolBytes;

    void Reset();

    PoolAllocator<LinearAllocator>* m_pFrameArena{nullptr};

    HeapVector<Allocation*> m_resources;

    /// stableId -> resource index in m_resources
    FlatHashMap<uint64_t, int32_t> m_resourceTable;

    /// Compatible descriptor -> available transient textures. Resource names are intentionally excluded.
    using TexturePool =
        FlatHashMap<RDGTexturePoolKey, HeapVector<PoolEntry>, RDGTexturePoolKeyHasher>;
    using BufferPool = FlatHashMap<RDGBufferPoolKey, HeapVector<PoolEntry>, RDGBufferPoolKeyHasher>;
    TexturePool m_texturePool;

    /// Compatible descriptor -> available transient buffers. Resource names are intentionally excluded.
    BufferPool m_bufferPool;
};
} // namespace zen::rc
