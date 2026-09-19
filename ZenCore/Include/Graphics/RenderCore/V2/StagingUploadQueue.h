#pragma once

#include "Graphics/RHI/RHIResource.h"
#include "Graphics/RenderCore/V2/RenderGraph/RenderGraph.h"
#include "Templates/HeapVector.h"
#include "Templates/VectorView.h"
#include <algorithm>
#include <cstdint>

namespace zen::rc
{
class RenderDevice;

enum class StagingFlushAction : uint32_t
{
    eNone  = 0,
    eFlush = 1
};

struct StagingAllocation
{
    RHIBuffer* pBuffer{nullptr};
    uint32_t offset{0};
    uint32_t size{0};
};

struct StagingCompletion
{
    uint64_t transferSerial{0};
    uint64_t graphicsSerial{0};

    void Extend(const StagingCompletion& other)
    {
        transferSerial = std::max(transferSerial, other.transferSerial);
        graphicsSerial = std::max(graphicsSerial, other.graphicsSerial);
    }

    bool IsCompleteAt(uint64_t completedTransfer, uint64_t completedGraphics) const
    {
        return completedTransfer >= transferSerial && completedGraphics >= graphicsSerial;
    }
};

// Append-only blocks. Both unsubmitted allocations and submitted GPU work prevent reuse.
class StagingBufferManager
{
public:
    explicit StagingBufferManager(uint32_t blockSize = 4 * 1024 * 1024,
                                  uint64_t poolSize  = 64 * 1024 * 1024);

    StagingBufferManager(const StagingBufferManager&) = delete;

    StagingBufferManager& operator=(const StagingBufferManager&) = delete;

    StagingFlushAction Allocate(uint32_t size, uint32_t alignment, StagingAllocation* pAllocation);

    void Release(const StagingAllocation& allocation, const StagingCompletion& completion);

    void Reclaim();

    // Wait for submitted blocks only; unsubmitted allocations still prevent reuse.
    bool WaitForSubmittedAllocations();

    void Destroy();

    uint32_t GetBlockSize() const
    {
        return m_blockSize;
    }

private:
    friend class StagingUploadQueue;

    void DestroyBuffer(RHIBuffer* buffer);

    // Set by the upload queue. Its device must outlive use of this manager.
    RenderDevice* m_pRenderDevice{nullptr};
    struct Block
    {
        RHIBuffer* pBuffer{nullptr};
        uint32_t capacity{0};
        uint32_t occupiedSize{0};
        uint32_t outstandingAllocCount{0};
        StagingCompletion completion{};
    };

    HeapVector<Block> m_blocks;
    uint32_t m_blockSize;
    uint64_t m_poolSize;
    uint64_t m_allocatedSize{0};
};

class StagingUploadQueue
{
public:
    StagingUploadQueue(RenderDevice* pRenderDevice, StagingBufferManager* pStagingMgr);

    StagingUploadQueue(const StagingUploadQueue&) = delete;

    StagingUploadQueue& operator=(const StagingUploadQueue&) = delete;

    // Try the final flush, then cancel any rejected pending uploads. Submitted work keeps
    // its completion gates; the device and staging manager must outlive this call.
    void Destroy();

    void EnqueueBuffer(RHIBuffer* pDstBuffer,
                       uint32_t dstOffset,
                       uint32_t dataSize,
                       const uint8_t* pData);

    // Validate payload bounds, copy alignment/format, and requested mip-blit capabilities
    // before staging. Reject the whole request without retaining or flushing earlier uploads.
    void EnqueueTexture(RHITexture* pTexture,
                        VectorView<RHIBufferTextureCopyRegion> regions,
                        uint32_t dataSize,
                        const uint8_t* pData,
                        bool generateMipmaps = false);

    // Success establishes submission, not GPU completion on timeline-capable backends.
    // Dependent graphs wait on the GPU; staging/resources remain retained until completion.
    // False retains unsubmitted payloads for retry and prevents dependent graph execution.
    bool Flush();

    void ReclaimResources();

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
        StagingAllocation stagingAlloc{};
        StagingCompletion attemptedCompletion{};
        RHIBuffer* pDstBuffer{nullptr};
        RHITexture* pDstTexture{nullptr};
        RHIBufferCopyRegion bufferCopyRegion{};
        uint32_t textureRegionOffset{0};
        uint32_t textureRegionCount{0};
        bool generateMipmaps{false};
    };

    struct RetainedResource
    {
        RHIResource* pResource{nullptr};
        StagingCompletion completion{};
    };

    bool StageBytes(uint32_t size,
                    uint32_t alignment,
                    const uint8_t* pData,
                    StagingAllocation* pOutAlloc);

    RenderDevice* m_pRenderDevice{nullptr};
    StagingBufferManager* m_pStagingMgr{nullptr};
    HeapVector<PendingUpload> m_pendingUploads;
    HeapVector<RHIBufferTextureCopyRegion> m_textureRegions;
    HeapVector<RetainedResource> m_retainedResources;
    RenderGraph m_uploadRDG;
    bool m_flushing{false};
};
} // namespace zen::rc
