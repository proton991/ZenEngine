#include "Graphics/RenderCore/V2/StagingUploadQueue.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RHI/DynamicRHI.h"
#include "RDGCopyValidation.h"
#include "Utils/Errors.h"

#include <cstring>
#include <limits>
#include <numeric>

namespace zen::rc
{
namespace
{
StagingCompletion SubmittedCompletion()
{
    return {GDynamicRHI->GetLastSubmittedSerial(RHICommandContextType::eTransfer),
            GDynamicRHI->GetLastSubmittedSerial(RHICommandContextType::eGraphics)};
}
} // namespace

StagingBufferManager::StagingBufferManager(uint32_t blockSize, uint64_t poolSize) :
    m_blockSize(blockSize), m_poolSize(std::max<uint64_t>(poolSize, blockSize))
{
    VERIFY_EXPR_MSG(blockSize > 0, "Staging block size must be positive");
}

StagingFlushAction StagingBufferManager::Allocate(uint32_t size,
                                                  uint32_t alignment,
                                                  StagingAllocation* pAllocation)
{
    VERIFY_EXPR_MSG(pAllocation != nullptr && size > 0 && alignment > 0,
                    "Invalid staging allocation request");
    *pAllocation              = {};
    StagingFlushAction action = StagingFlushAction::eFlush;

    if (m_pRenderDevice == nullptr || !m_pRenderDevice->AreSubmissionsBlocked())
    {
        Reclaim();

        for (Block& block : m_blocks)
        {
            const uint64_t offset =
                (uint64_t(block.occupiedSize) + alignment - 1) / alignment * alignment;

            if (offset + size <= block.capacity)
            {
                block.occupiedSize = static_cast<uint32_t>(offset + size);
                ++block.outstandingAllocCount;
                *pAllocation = {block.pBuffer, static_cast<uint32_t>(offset), size};
                action       = StagingFlushAction::eNone;
                break;
            }
        }

        if (action != StagingFlushAction::eNone)
        {
            const uint32_t capacity = std::max(size, m_blockSize);
            // An individual texture may exceed the normal pool budget. Only one such block
            // is admitted, after all other blocks have drained.
            const uint64_t budget = std::max<uint64_t>(m_poolSize, capacity);

            for (size_t i = 0; i < m_blocks.size() && m_allocatedSize + capacity > budget;)
            {
                if (m_blocks[i].occupiedSize == 0)
                {
                    m_allocatedSize -= m_blocks[i].capacity;
                    DestroyBuffer(m_blocks[i].pBuffer);
                    m_blocks.erase(m_blocks.begin() + i);
                }
                else
                {
                    ++i;
                }
            }

            if (m_allocatedSize + capacity <= budget)
            {
                RHIBufferCreateInfo info{};
                info.size = capacity;
                info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferSrcBuffer);
                info.allocateType  = RHIBufferAllocateType::eCPUWrite;
                info.tag           = "staging_upload";
                RHIBuffer* pBuffer = GDynamicRHI->CreateBuffer(info);
                VERIFY_EXPR_MSG(pBuffer != nullptr, "Failed to allocate a staging buffer");
                m_blocks.push_back({pBuffer, capacity, size, 1, {}});
                m_allocatedSize += capacity;
                *pAllocation = {pBuffer, 0, size};
                action       = StagingFlushAction::eNone;
            }
        }
    }

    return action;
}

void StagingBufferManager::Release(const StagingAllocation& allocation,
                                   const StagingCompletion& completion)
{
    for (Block& block : m_blocks)
    {
        if (block.pBuffer == allocation.pBuffer)
        {
            VERIFY_EXPR_MSG(block.outstandingAllocCount > 0, "Staging allocation released twice");
            --block.outstandingAllocCount;
            block.completion.Extend(completion);

            return;
        }
    }

    VERIFY_EXPR_MSG(false, "Staging allocation does not belong to this manager");
}

void StagingBufferManager::Reclaim()
{
    const uint64_t transfer = GDynamicRHI->GetLastCompletedSerial(RHICommandContextType::eTransfer);
    const uint64_t graphics = GDynamicRHI->GetLastCompletedSerial(RHICommandContextType::eGraphics);

    for (Block& block : m_blocks)
    {
        if (block.outstandingAllocCount == 0 && block.completion.IsCompleteAt(transfer, graphics))
        {
            block.occupiedSize = 0;
            block.completion   = {};
        }
    }
}

bool StagingBufferManager::WaitForSubmittedAllocations()
{
    bool valid = true;

    StagingCompletion required{};

    for (StagingBufferManager::Block const& block : m_blocks)
    {
        if (block.outstandingAllocCount == 0)
        {
            required.Extend(block.completion);
        }
    }

    for (const std::pair<RHICommandContextType, uint64_t>& completion :
         {std::pair{RHICommandContextType::eGraphics, required.graphicsSerial},
          std::pair{RHICommandContextType::eTransfer, required.transferSerial}})
    {
        if (GDynamicRHI->GetLastCompletedSerial(completion.first) < completion.second &&
            !GDynamicRHI->WaitForSubmission(completion.first, completion.second))
        {
            LOGE("Staging: cannot reclaim queue {} serial {}", uint32_t(completion.first),
                 completion.second);
            valid = false;
        }

        if (!valid)
        {
            break;
        }
    }

    if (valid)
    {
        Reclaim();
    }

    return valid;
}

void StagingBufferManager::Destroy()
{
    if (!m_blocks.empty())
    {
        GDynamicRHI->WaitDeviceIdle();
    }

    for (const Block& block : m_blocks)
    {
        VERIFY_EXPR_MSG(block.outstandingAllocCount == 0, "Unsubmitted staging allocations remain");
        DestroyBuffer(block.pBuffer);
    }

    m_blocks.clear();
    m_allocatedSize = 0;
}

void StagingBufferManager::DestroyBuffer(RHIBuffer* buffer)
{
    if (m_pRenderDevice != nullptr)
    {
        m_pRenderDevice->InvalidateExternalBufferState(buffer);
    }

    GDynamicRHI->DestroyBuffer(buffer);
}

StagingUploadQueue::StagingUploadQueue(RenderDevice* pRenderDevice,
                                       StagingBufferManager* pStagingMgr) :
    m_pRenderDevice(pRenderDevice), m_pStagingMgr(pStagingMgr), m_uploadRDG("staging_upload")
{
    VERIFY_EXPR_MSG(pRenderDevice != nullptr && pStagingMgr != nullptr,
                    "Invalid upload queue dependencies");
    VERIFY_EXPR_MSG(pStagingMgr->m_pRenderDevice == nullptr ||
                        pStagingMgr->m_pRenderDevice == pRenderDevice,
                    "A staging manager cannot be shared by different render devices");
    pStagingMgr->m_pRenderDevice = pRenderDevice;
}

bool StagingUploadQueue::StageBytes(uint32_t size,
                                    uint32_t alignment,
                                    const uint8_t* pData,
                                    StagingAllocation* pOutAlloc)
{
    bool valid = true;

    valid = !(size == 0 || pData == nullptr || pOutAlloc == nullptr);

    if (valid)
    {
        StagingFlushAction action = m_pStagingMgr->Allocate(size, alignment, pOutAlloc);

        if (action != StagingFlushAction::eNone)
        {
            // This queue may be used independently of the device's main upload queue.
            Flush();
            valid = m_pRenderDevice->ResolveStagingFlushAction(action, m_pStagingMgr);

            if (valid)
            {
                action = m_pStagingMgr->Allocate(size, alignment, pOutAlloc);
            }
        }

        if (valid)
        {
            if (action != StagingFlushAction::eNone)
            {
                LOGE("Staging pool still has outstanding allocations after flushing");
                valid = false;
            }
        }

        if (valid)
        {
            uint8_t* pMapped = pOutAlloc->pBuffer->Map();

            if (pMapped == nullptr)
            {
                m_pStagingMgr->Release(*pOutAlloc, {});
                *pOutAlloc = {};
                valid      = false;
            }

            if (valid)
            {
                std::memcpy(pMapped + pOutAlloc->offset, pData, size);
                pOutAlloc->pBuffer->Unmap();
            }
        }
    }

    return valid;
}

void StagingUploadQueue::EnqueueBuffer(RHIBuffer* pDstBuffer,
                                       uint32_t dstOffset,
                                       uint32_t dataSize,
                                       const uint8_t* pData)
{
    if (dataSize == 0 || pData == nullptr)
    {
        return;
    }

    VERIFY_EXPR_MSG(pDstBuffer != nullptr, "Null upload destination buffer");
    VERIFY_EXPR_MSG(uint64_t(dstOffset) + dataSize <= pDstBuffer->GetRequiredSize(),
                    "Buffer upload is out of bounds");

    for (uint32_t copied = 0; copied < dataSize;)
    {
        const uint32_t size = std::min(dataSize - copied, m_pStagingMgr->GetBlockSize());
        PendingUpload upload{};
        const bool staged = StageBytes(size, 4, pData + copied, &upload.stagingAlloc);

        if (!staged)
        {
            LOGE("Failed to stage buffer upload");
            break;
        }

        upload.pDstBuffer       = pDstBuffer;
        upload.bufferCopyRegion = {upload.stagingAlloc.offset, uint64_t(dstOffset) + copied, size};
        pDstBuffer->AddReference();
        m_pendingUploads.push_back(std::move(upload));
        copied += size;
    }
}

void StagingUploadQueue::EnqueueTexture(RHITexture* pTexture,
                                        VectorView<RHIBufferTextureCopyRegion> regions,
                                        uint32_t dataSize,
                                        const uint8_t* pData,
                                        bool generateMipmaps)
{
    if (dataSize == 0 || pData == nullptr || regions.empty())
    {
        return;
    }

    if (pTexture == nullptr || regions.data() == nullptr)
    {
        LOGE("Texture upload [{}]: Null destination texture or copy regions",
             uint32_t(RDGErrorCode::eRange));
    }
    else
    {
        // Validate the entire request before StageBytes can allocate or flush earlier uploads.
        // RDG later sees the whole staging buffer; only this boundary knows the payload's size.
        RDGResult result;
        const RHITextureCreateInfo& info = pTexture->GetBaseInfo();
        bool requiresGraphics            = false;
        uint32_t alignment               = 16;

        if (!result.Check(info.usageFlags.HasFlag(RHITextureUsageFlagBits::eTransferDst) &&
                              (!generateMipmaps ||
                               info.usageFlags.HasFlag(RHITextureUsageFlagBits::eTransferSrc)),
                          RDGErrorCode::eBinding,
                          "Texture upload lacks required transfer creation usage") ||
            (generateMipmaps && !ValidateMipmapCapabilities(result, info)))
        {
            LOGE("Texture upload '{}' [{}]: {}", pTexture->GetResourceTag().CStr(),
                 uint32_t(result.code), result.message);
        }
        else
        {
            for (size_t i = 0; i < regions.size(); ++i)
            {
                RHIBufferTextureCopyRegion const& region = regions[i];

                if (!ValidateTextureCopyBox(result, pTexture->GetBaseInfo(),
                                            pTexture->GetResourceTag(), region.textureSubresources,
                                            region.textureOffset, region.textureSize) ||
                    !ValidateBufferTextureFootprint(result, dataSize, info, region) ||
                    !ValidateBufferTextureCopyCapabilities(result, info, region, requiresGraphics))
                {
                    LOGE("Texture upload '{}' region {} [{}]: {} (payload bytes: {})",
                         pTexture->GetResourceTag().CStr(), i, uint32_t(result.code),
                         result.message, dataSize);
                    break;
                }

                alignment = std::lcm(alignment, BufferTextureCopyAlignment(info, region));
            }

            if (result)
            {
                PendingUpload upload{};
                // Rebasing must preserve texel alignment, including non-power-of-two RGB formats.
                const bool staged = StageBytes(dataSize, alignment, pData, &upload.stagingAlloc);

                if (!staged)
                {
                    LOGE("Failed to stage texture upload");
                }
                else
                {
                    upload.pDstTexture         = pTexture;
                    upload.generateMipmaps     = generateMipmaps;
                    upload.textureRegionOffset = static_cast<uint32_t>(m_textureRegions.size());
                    upload.textureRegionCount  = static_cast<uint32_t>(regions.size());

                    for (RHIBufferTextureCopyRegion region : regions)
                    {
                        region.bufferOffset += upload.stagingAlloc.offset;
                        m_textureRegions.push_back(region);
                    }

                    pTexture->AddReference();
                    m_pendingUploads.push_back(std::move(upload));
                }
            }
        }
    }
}

bool StagingUploadQueue::Flush()
{
    bool result{};

    if (m_flushing)
    {
        result = true;
    }
    else
    {
        ReclaimResources();

        if (!HasPending())
        {
            result = true;
        }
        else
        {
            m_flushing = true;

            struct FlushGuard
            {
                bool& flag;

                ~FlushGuard()
                {
                    flag = false;
                }
            } guard{m_flushing};

            m_uploadRDG.Begin();

            for (const PendingUpload& upload : m_pendingUploads)
            {
                // StageBytes filled every copied range in coherent CPU-write memory. Submission
                // makes those writes visible; the staging manager protects in-flight allocations.
                m_uploadRDG.GetResourceManager()->ImportHostWrittenBuffer(
                    upload.stagingAlloc.pBuffer);

                {
                    RDGTransferPassCmdRecorder pass = m_uploadRDG.AddTransferPass("upload_copy");

                    if (upload.pDstBuffer != nullptr)
                    {
                        pass.CopyBuffer(upload.stagingAlloc.pBuffer, upload.pDstBuffer,
                                        upload.bufferCopyRegion);
                    }
                    else
                    {
                        for (uint32_t i = 0; i < upload.textureRegionCount; ++i)
                        {
                            pass.CopyBufferToTexture(
                                upload.stagingAlloc.pBuffer, upload.pDstTexture,
                                m_textureRegions[upload.textureRegionOffset + i]);
                        }
                    }
                }

                if (upload.generateMipmaps)
                {
                    m_uploadRDG.AddTransferPass("upload_mipmaps")
                        .GenerateMipmaps(upload.pDstTexture);
                }
            }

            m_uploadRDG.End();

            if (!m_pRenderDevice->ExecuteRenderGraph(m_uploadRDG))
            {
                // A failed completion wait or partial submission may still have accepted GPU work.
                // Preserve its gates if teardown later cancels these retained allocations.
                const StagingCompletion completion = SubmittedCompletion();

                for (StagingUploadQueue::PendingUpload& upload : m_pendingUploads)
                {
                    upload.attemptedCompletion.Extend(completion);
                }

                m_flushing = false;
                result     = false;
            }
            else
            {
                const StagingCompletion completion = SubmittedCompletion();

                for (const PendingUpload& upload : m_pendingUploads)
                {
                    m_pStagingMgr->Release(upload.stagingAlloc, completion);
                    RHIResource* pResource = upload.pDstBuffer != nullptr ?
                        static_cast<RHIResource*>(upload.pDstBuffer) :
                        upload.pDstTexture;
                    m_retainedResources.push_back({pResource, completion});
                }

                m_pendingUploads.clear();
                m_textureRegions.clear();
                // Upload graphs are single-use; retire their import references after submission.
                m_uploadRDG.Reset();
                m_flushing = false;
                ReclaimResources();
                result = true;
            }
        }
    }

    return result;
}

void StagingUploadQueue::ReclaimResources()
{
    const uint64_t transfer = GDynamicRHI->GetLastCompletedSerial(RHICommandContextType::eTransfer);
    const uint64_t graphics = GDynamicRHI->GetLastCompletedSerial(RHICommandContextType::eGraphics);

    for (size_t i = 0; i < m_retainedResources.size();)
    {
        if (m_retainedResources[i].completion.IsCompleteAt(transfer, graphics))
        {
            m_retainedResources[i].pResource->ReleaseReference();
            m_retainedResources.erase(m_retainedResources.begin() + i);
        }
        else
        {
            ++i;
        }
    }

    m_pStagingMgr->Reclaim();
}

void StagingUploadQueue::Destroy()
{
    if (m_flushing)
    {
        LOGE("Staging [{}]: Cannot destroy an upload queue while it is flushing",
             uint32_t(RDGErrorCode::eLifecycle));
    }
    else
    {
        Flush();

        // Drop recorded callbacks/imports before cancelling the unsubmitted payloads they reference.
        if (m_uploadRDG.Reset())
        {
            for (const PendingUpload& upload : m_pendingUploads)
            {
                // Release only this allocation; a shared block may still have submitted or pending users.
                m_pStagingMgr->Release(upload.stagingAlloc, upload.attemptedCompletion);
                RHIResource* resource = upload.pDstBuffer != nullptr ?
                    static_cast<RHIResource*>(upload.pDstBuffer) :
                    upload.pDstTexture;
                m_pRenderDevice->DeferReleaseResource(resource);
            }

            m_pendingUploads.clear();
            m_textureRegions.clear();

            if (!m_retainedResources.empty())
            {
                GDynamicRHI->WaitDeviceIdle();
                ReclaimResources();
            }

            VERIFY_EXPR(m_retainedResources.empty());
            m_pStagingMgr->Reclaim();
        }
    }
}
} // namespace zen::rc
