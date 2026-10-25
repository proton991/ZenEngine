#pragma once

#include "Graphics/RHI/RHIResource.h"
#include "Graphics/RenderCore/V2/RenderGraph/RenderGraph.h"
#include "Graphics/RenderCore/V2/ResourceRetirement.h"
#include "Templates/HeapVector.h"
#include "Templates/VectorView.h"
#include <algorithm>
#include <cstdint>

namespace zen::rc
{
class RenderDevice;

enum class StagingFlushAction : uint32_t
{
    eNone   = 0,
    eFlush  = 1,
    eFailed = 2
};

struct StagingAllocation
{
    RHIBuffer* pBuffer{nullptr};
    uint32_t   offset{0};
    uint32_t   size{0};
};

// Render-thread observations. Occupied bytes include alignment padding and
// submitted ranges awaiting retirement; capacity includes reusable empty blocks.
struct StagingPoolStatistics
{
    uint64_t capacityBytes{0};
    uint64_t peakCapacityBytes{0};
    uint64_t occupiedBytes{0};
    uint64_t peakOccupiedBytes{0};
    uint64_t allocationBytes{0};
    uint64_t peakAllocationBytes{0};
    uint32_t blockCount{0};
    uint32_t peakBlockCount{0};
    // False if any observed block lacked native allocation reporting since reset.
    bool     allocationBytesAvailable{true};
    uint64_t allocationFlushRequests{0};
    uint64_t allocationFailures{0};
    uint64_t waitCalls{0};
    uint64_t waitFailures{0};
    // CPU time within WaitForSubmittedAllocations; excludes the preceding flush.
    uint64_t waitCPUTimeNS{0};
};

// Append-only blocks. Both unsubmitted allocations and submitted GPU work prevent reuse.
class StagingBufferManager
{
public:
    explicit StagingBufferManager(uint32_t blockSize = 4 * 1024 * 1024, uint64_t poolSize = 64 * 1024 * 1024);

    StagingBufferManager(const StagingBufferManager&)            = delete;

    StagingBufferManager& operator=(const StagingBufferManager&) = delete;

    StagingFlushAction Allocate(uint32_t size, uint32_t alignment, StagingAllocation* pAllocation);

    void Release(const StagingAllocation& allocation, const ResourceRetirement& retirement);

    void Reclaim();

    // Wait for submitted blocks only; unsubmitted allocations still prevent reuse.
    bool WaitForSubmittedAllocations();

    void Destroy();

    StagingPoolStatistics GetStatistics() const
    {
        return m_statistics;
    }

    // Starts an observation interval with existing live/occupied blocks included.
    // Neither this call nor GetStatistics reclaims, submits or waits for work.
    void ResetStatistics();

    uint32_t GetBlockSize() const
    {
        return m_blockSize;
    }

private:
    friend class StagingUploadQueue;

    void DestroyBuffer(RHIBuffer* buffer);

    void UpdateStatistics();

    // Set by the upload queue. Its device must outlive use of this manager.
    RenderDevice* m_pRenderDevice{nullptr};
    struct Block
    {
        RHIBuffer*         pBuffer{nullptr};
        uint32_t           capacity{0};
        uint32_t           occupiedSize{0};
        uint32_t           outstandingAllocCount{0};
        ResourceRetirement retirement;
    };

    HeapVector<Block>     m_blocks;
    uint32_t              m_blockSize;
    uint64_t              m_poolSize;
    uint64_t              m_allocatedSize{0};
    StagingPoolStatistics m_statistics;
};

// VERIFY_EXPR* preconditions abort in both Debug and Release. Dependencies must be initialized
// and buffer payloads/ranges validated by the content-facing caller. A false result reports
// staging/submission failure, not recovery from a violated programmer precondition.
class StagingUploadQueue
{
public:
    StagingUploadQueue(RenderDevice* pRenderDevice, StagingBufferManager* pStagingMgr);

    StagingUploadQueue(const StagingUploadQueue&)            = delete;

    StagingUploadQueue& operator=(const StagingUploadQueue&) = delete;

    // Try the final flush, then cancel any rejected pending uploads. Submitted work keeps
    // its completion gates; the device and staging manager must outlive this call.
    void Destroy();

    // A nonempty payload requires a live destination and offset + size within its allocation.
    // The caller supplies at least dataSize readable bytes. Null/empty payloads are no-ops.
    bool EnqueueBuffer(RHIBuffer* pDstBuffer, uint32_t dstOffset, uint32_t dataSize, const uint8_t* pData);

    // Validate payload bounds, copy alignment/format, and requested mip-blit capabilities
    // before staging. Reject the whole request without retaining or flushing earlier uploads.
    bool EnqueueTexture(RHITexture*                            pTexture,
                        VectorView<RHIBufferTextureCopyRegion> regions,
                        uint32_t                               dataSize,
                        const uint8_t*                         pData,
                        bool                                   generateMipmaps = false);

    // Success establishes queued handoff, not CPU or GPU completion on threaded timeline backends.
    // Dependent graphs wait on the GPU; staging/resources remain retained until completion.
    // Rejection before handoff retains payloads for retry; failure after handoff blocks dependent work.
    bool Flush();

    void ReclaimResources();

    // Bytes copied into staging, including partial requests; a submission retry does not add bytes.
    uint64_t GetStagedBytes() const
    {
        return m_stagedBytes;
    }

    bool HasPending() const
    {
        return !m_pendingUploads.empty();
    }

    // Preserve the spelling in the original declaration.
    bool HashPending() const
    {
        return HasPending();
    }

private:
    struct PendingUpload
    {
        StagingAllocation   stagingAlloc{};
        ResourceRetirement  attemptedRetirement;
        RHIBuffer*          pDstBuffer{nullptr};
        RHITexture*         pDstTexture{nullptr};
        RHIBufferCopyRegion bufferCopyRegion{};
        uint32_t            textureRegionOffset{0};
        uint32_t            textureRegionCount{0};
        bool                generateMipmaps{false};
    };

    struct RetainedResource
    {
        RHIResource*       pResource{nullptr};
        ResourceRetirement retirement;
    };

    bool StageBytes(uint32_t size, uint32_t alignment, const uint8_t* pData, StagingAllocation* pOutAlloc);

    RenderDevice*                          m_pRenderDevice{nullptr};
    StagingBufferManager*                  m_pStagingMgr{nullptr};
    HeapVector<PendingUpload>              m_pendingUploads;
    HeapVector<RHIBufferTextureCopyRegion> m_textureRegions;
    HeapVector<RetainedResource>           m_retainedResources;
    RenderGraph                            m_uploadRDG;
    bool                                   m_flushing{false};
    uint64_t                               m_stagedBytes{0};
};
} // namespace zen::rc
