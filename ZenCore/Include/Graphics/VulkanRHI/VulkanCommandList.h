#pragma once
#include "VulkanCommandList.h"
#include "VulkanQueue.h"
#include "VulkanRHI.h"
#include "Graphics/RHI/RHICommandList.h"
#include "Graphics/VulkanRHI/VulkanHeaders.h"
#include "Templates/HeapVector.h"
#include "Utils/Mutex.h"

#ifndef ZEN_VK_RHI_DEBUG
#define ZEN_VK_RHI_DEBUG 0
#endif

namespace zen
{
class VulkanDescriptorSet;
class VulkanBuffer;
class VulkanPipeline;
class FVulkanCommandBufferPool;
class VulkanFence;
class VulkanSemaphore;
class VulkanQueue;
struct RHIRenderingLayout;
class FVulkanCommandListContext;

class FVulkanCommandBuffer
{
public:
    enum class State : uint8_t
    {
        eReadyForBegin,
        eIsInsideBegin,
        eIsInsideRenderPass,
        eHasEnded,
        eSubmitted,
        eNotAllocated,
        eNeedReset,
    };

    void Begin();

    void End();

    void BeginRendering(const VkRenderingInfo* pRenderingInfo);

    void EndRendering();

    void BeginRenderPass(const VkRenderPassBeginInfo* pBeginInfo);

    void EndRenderPass();

    void SetSubmitted();

    void SetCompleted();

    bool IsInsideRenderPass() const
    {
        return m_state == State::eIsInsideRenderPass;
    }

    bool IsOutsideRenderPass() const
    {
        return m_state == State::eIsInsideBegin;
    }

    bool HasBegun() const
    {
        return m_state == State::eIsInsideBegin || m_state == State::eIsInsideRenderPass;
    }

    bool HasEnded() const
    {
        return m_state == State::eHasEnded;
    }

    bool IsSubmitted() const
    {
        return m_state == State::eSubmitted;
    }

    bool IsAllocated() const
    {
        return m_state != State::eNotAllocated;
    }

    VkCommandBuffer GetVkHandle() const
    {
        return m_vkHandle;
    }

    FVulkanCommandBufferPool* GetCommandBufferPool() const
    {
        return m_pCmdBufferPool;
    }

    VulkanCommandBufferType GetCommandBufferType() const;

protected:
    friend class FVulkanCommandBufferPool;
    friend class VulkanCommandContextBase;

    FVulkanCommandBuffer(FVulkanCommandBufferPool* pPool);
    ~FVulkanCommandBuffer();

private:
    void AllocMemory();

    void FreeMemory();

    VkCommandBuffer m_vkHandle{VK_NULL_HANDLE};

    FVulkanCommandBufferPool* m_pCmdBufferPool{nullptr};

    State m_state{State::eNotAllocated};

    double m_submitTime{0.0f};

    VkRenderingFlags m_lastRenderingFlags{0};
};


class FVulkanCommandBufferPool
{
    friend class VulkanCommandContextBase;

public:
    FVulkanCommandBufferPool(VulkanQueue* pQueue, VulkanCommandBufferType type);

    ~FVulkanCommandBufferPool();

    void FreeUnusedCommandBuffers();

    Mutex* GetMutex()
    {
        return &m_mutex;
    }

    VkCommandPool GetVkHandle() const
    {
        return m_vkHandle;
    }

    VulkanQueue* GetQueue() const
    {
        return m_pQueue;
    }

    VulkanCommandBufferType GetCommandBufferType() const
    {
        return m_type;
    }

private:
    FVulkanCommandBuffer* CreateCmdBuffer();

    Mutex m_mutex;

    VulkanQueue* m_pQueue{nullptr};

    VulkanCommandBufferType m_type;

    VkCommandPool m_vkHandle{VK_NULL_HANDLE};

    HeapVector<FVulkanCommandBuffer*> m_cmdBuffersInUse;
    HeapVector<FVulkanCommandBuffer*> m_cmdBuffersFree;

#if ZEN_VK_RHI_DEBUG
    uint64_t m_numCmdBufferRequests{0};
    uint64_t m_numReadyCmdBufferReuses{0};
    uint64_t m_numFreeCmdBufferReuses{0};
    uint64_t m_numCmdBufferAllocations{0};
#endif
};

// Hodls submission info for VulkanQueue
// In the future, adds GPU profiling related metrics in this structure
class VulkanWorkload
{
    friend class VulkanCommandContextBase;
    friend class VulkanQueue;
    friend class VulkanRHI;

public:
    explicit VulkanWorkload(VulkanQueue* pQueue) : m_pQueue(pQueue) {}

    ~VulkanWorkload();

    struct WaitSemaphoreInfo
    {
        VkPipelineStageFlags waitFlags{0};
        VulkanSemaphore* pSemaphore{nullptr};
        uint64_t value{0};
    };

    struct SignalSemaphoreInfo
    {
        VulkanSemaphore* pSemaphore{nullptr};
        uint64_t value{0};
    };

private:
    FVulkanCommandBuffer* GetLastCommandBuffer() const;

    bool HasCommandBuffers() const
    {
        return !m_commandBuffers.empty();
    }

    void AddCommandBuffer(FVulkanCommandBuffer* pCmdBuffer);

    void Merge(VulkanWorkload* pOtherWorkload);

    VulkanQueue* m_pQueue{nullptr};
    HeapVector<FVulkanCommandBuffer*> m_commandBuffers;
    uint64_t m_submissionSerial{0};
    VulkanWorkload* m_pMergedInto{nullptr};
    // DO NOT own the semaphores, only hold reference
    HeapVector<WaitSemaphoreInfo> m_waitSemaphoreInfos;
    HeapVector<SignalSemaphoreInfo> m_signalSemaphoreInfos;

    VulkanFence* m_pFence{nullptr}; // Used at vkQueueSubmit
};



// manages VulkanWorkload
// holds VulkanCommandBufferPool
class VulkanCommandContextBase
{
public:
    VulkanCommandContextBase(VulkanQueue* pQueue, VulkanCommandBufferType type) :
        m_pQueue(pQueue), m_pCmdBufferPool(pQueue->AcquireCommandBufferPool(type))
    {}

    virtual ~VulkanCommandContextBase();

    FVulkanCommandBuffer* GetCommandBuffer()
    {
        VulkanWorkload* pWorkload = GetWorkload(WorkloadPhase::eExecute);
        if (!pWorkload->HasCommandBuffers())
        {
            SetupNewCommandBuffer();
        }

        return pWorkload->GetLastCommandBuffer();
    }

    void AddWaitSemaphore(VkPipelineStageFlags waitFlags, VulkanSemaphore* pWaitSemaphore)
    {
        AddWaitSemaphore(waitFlags, pWaitSemaphore, 0);
    }

    void AddWaitSemaphore(VkPipelineStageFlags waitFlags,
                          VulkanSemaphore* pWaitSemaphore,
                          uint64_t value)
    {
        VulkanWorkload* pCurrentWorkload = GetWorkload(WorkloadPhase::eWait);
        pCurrentWorkload->m_waitSemaphoreInfos.emplace_back(
            VulkanWorkload::WaitSemaphoreInfo{waitFlags, pWaitSemaphore, value});
    }

    void AddWaitSemaphores(VkPipelineStageFlags waitFlags,
                           VectorView<VulkanSemaphore*> waitSemaphores)
    {
        VulkanWorkload* pCurrentWorkload = GetWorkload(WorkloadPhase::eWait);
        for (uint32_t i = 0; i < waitSemaphores.size(); i++)
        {
            pCurrentWorkload->m_waitSemaphoreInfos.emplace_back(
                VulkanWorkload::WaitSemaphoreInfo{waitFlags, waitSemaphores[i], 0});
        }
    }

    void AddSignalSemaphore(VulkanSemaphore* pSignalSemaphore)
    {
        AddSignalSemaphore(pSignalSemaphore, 0);
    }

    void AddSignalSemaphore(VulkanSemaphore* pSignalSemaphore, uint64_t value)
    {
        VulkanWorkload* pCurrentWorkload = GetWorkload(WorkloadPhase::eSignal);
        pCurrentWorkload->m_signalSemaphoreInfos.emplace_back(
            VulkanWorkload::SignalSemaphoreInfo{pSignalSemaphore, value});
    }

    void AddSignalSemaphores(VectorView<VulkanSemaphore*> signalSemaphores)
    {
        VulkanWorkload* pCurrentWorkload = GetWorkload(WorkloadPhase::eSignal);
        for (uint32_t i = 0; i < signalSemaphores.size(); i++)
        {
            pCurrentWorkload->m_signalSemaphoreInfos.emplace_back(
                VulkanWorkload::SignalSemaphoreInfo{signalSemaphores[i], 0});
        }
    }

    // Finalize the current workload, then append all staged workloads to the output array.
    void CollectWorkloads(HeapVector<VulkanWorkload*>& outWorkloads);

    // Finalize the current workload and submit all staged workloads immediately.
    void SubmitRecordedWorkloads();

    void WaitForLastSubmittedWork(uint64_t timeToWaitNS);

    VulkanQueue* GetQueue() const
    {
        return m_pQueue;
    }

    void SetLastSubmittedSerial(uint64_t submissionSerial)
    {
        m_lastSubmittedSerial = submissionSerial;
    }

    uint64_t GetLastSubmittedSerial() const
    {
        return m_lastSubmittedSerial;
    }

private:
    enum class WorkloadPhase : uint8_t
    {
        eWait,
        eExecute,
        eSignal,
    };

    VulkanWorkload* GetWorkload(WorkloadPhase phase);

    bool HasWorkloadData(const VulkanWorkload* pWorkload) const;

    void FinalizePendingWorkload();

    void SetupNewCommandBuffer();

    void StartWorkload();

    void EndWorkload();

    VulkanQueue* m_pQueue{nullptr};

    FVulkanCommandBufferPool* m_pCmdBufferPool{nullptr};

    VulkanWorkload* m_pCurrentWorkload{nullptr};
    HeapVector<VulkanWorkload*> m_finalizedWorkloads;
    WorkloadPhase m_currentWorkloadPhase{WorkloadPhase::eWait};
    uint64_t m_lastSubmittedSerial{0};
};


class VulkanGfxState
{
public:
    void SetViewport(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY);

    void SetScissor(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY);

    void SetBlendConstants(float r, float g, float b, float a);

    void SetLineWidth(float lineWidth);

    void SetDepthBias(float depthBiasConstantFactor,
                      float depthBiasClamp,
                      float depthBiasSlopeFactor);

    void SetVertexBuffers(uint32_t numVertexBuffers,
                          RHIBuffer* const* ppVertexBuffers,
                          const uint64_t* pOffsets);

    void SetPipelineState(RHIPipeline* pPipeline,
                          uint32_t numDescriptorSets,
                          RHIDescriptorSet* const* ppDescriptorSets);

    void PreDraw(FVulkanCommandListContext* pContext);

private:
    HeapVector<VkViewport> m_viewports;
    HeapVector<VkRect2D> m_scissors;

    VulkanPipeline* m_pCurrentPipeline{nullptr};
    HeapVector<VkDescriptorSet> m_descriptorSets;

    HeapVector<VkBuffer> m_vertexBuffers;
    HeapVector<uint64_t> m_vertexBufferOffsets;

    struct RasterizationStates
    {
        bool depthBiasEnable{false};
        float depthBiasConstantFactor{0.0f};
        float depthBiasClamp{0.0f};
        float depthBiasSlopeFactor{0.0f};
        float lineWidth{1.0f};
    };

    RasterizationStates m_rasterizationState{};

    float m_blendConstants[4]{};
};

class VulkanComputeState
{
public:
    void SetPipelineState(RHIPipeline* pPipeline,
                          uint32_t numDescriptorSets,
                          RHIDescriptorSet* const* ppDescriptorSets);

    void PreDispatch(FVulkanCommandListContext* pContext);

private:
    VulkanPipeline* m_pCurrentPipeline{nullptr};
    HeapVector<VkDescriptorSet> m_descriptorSets;
};

class FVulkanCommandListContext : public IRHICommandContext, public VulkanCommandContextBase
{
public:
    FVulkanCommandListContext(RHICommandContextType contextType, VulkanDevice* pDevice);

    virtual ~FVulkanCommandListContext();

    RHICommandContextType GetContextType() override;

    void RHIBeginRendering(const RHIRenderingLayout* pRenderingLayout) override;

    void RHIEndRendering() override;

    void RHISetScissor(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY) override;

    void RHISetViewport(uint32_t minX, uint32_t minY, uint32_t maxX, uint32_t maxY) override;

    void RHISetDepthBias(float depthBiasConstantFactor,
                         float depthBiasClamp,
                         float depthBiasSlopeFactor) override;

    void RHISetLineWidth(float lineWidth) override;

    void RHISetBlendConstants(const Color& blendConstants) override;

    void RHIBindPipeline(RHIPipeline* pPipeline,
                         uint32_t numDescriptorSets,
                         RHIDescriptorSet* const* pDescriptorSets) override;

    void RHIBindVertexBuffers(VectorView<RHIBuffer*> pBuffers,
                              VectorView<uint64_t> offsets) override;

    void RHIBindVertexBuffer(RHIBuffer* pBuffer, uint64_t offset) override;

    void RHIDraw(uint32_t vertexCount,
                 uint32_t instanceCount,
                 uint32_t firstVertex,
                 uint32_t firstInstance) override;

    void RHIDrawIndexed(RHIBuffer* pIndexBuffer,
                        DataFormat indexFormat,
                        uint32_t indexBufferOffset,
                        uint32_t indexCount,
                        uint32_t instanceCount,
                        uint32_t firstIndex,
                        int32_t vertexOffset,
                        uint32_t firstInstance) override;

    void RHIDrawIndexedIndirect(RHIBuffer* pIndirectBuffer,
                                RHIBuffer* pIndexBuffer,
                                DataFormat indexFormat,
                                uint32_t indexBufferOffset,
                                uint32_t offset,
                                uint32_t drawCount,
                                uint32_t stride) override;

    void RHIDispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override;

    void RHIDispatchIndirect(RHIBuffer* pIndirectBuffer, uint32_t offset) override;

    void RHISetPushConstants(RHIPipeline* pPipeline, VectorView<uint8_t> data) override;

    void RHIAddTransitions(BitField<RHIPipelineStageBits> srcStages,
                           BitField<RHIPipelineStageBits> dstStages,
                           VectorView<RHIMemoryTransition> memoryTransitions,
                           VectorView<RHIBufferTransition> bufferTransitions,
                           VectorView<RHITextureTransition> textureTransitions) override;

    void RHIGenTextureMipmaps(RHITexture* pTexture) override;

    void RHIAddTextureTransition(RHITexture* pTexture, RHITextureLayout newLayout) override;

    void RHIClearBuffer(RHIBuffer* pBuffer, uint32_t offset, uint32_t size) override;

    void RHICopyBuffer(RHIBuffer* pSrcBuffer,
                       RHIBuffer* pDstBuffer,
                       const RHIBufferCopyRegion& region) override;

    void RHIClearTexture(RHITexture* pTexture,
                         const Color& color,
                         const RHITextureSubResourceRange& range) override;

    void RHICopyTexture(RHITexture* pSrcTexture,
                        RHITexture* pDstTexture,
                        VectorView<RHITextureCopyRegion> regions) override;

    void RHICopyTextureToBuffer(RHITexture* pSrcTex,
                                RHIBuffer* pDstBuffer,
                                VectorView<RHIBufferTextureCopyRegion> regions) override;

    void RHICopyBufferToTexture(RHIBuffer* pSrcBuffer,
                                RHITexture* pDstTexture,
                                VectorView<RHIBufferTextureCopyRegion> regions) override;

    void RHIResolveTexture(RHITexture* pSrcTexture,
                           RHITexture* pDstTexture,
                           uint32_t srcLayer,
                           uint32_t srcMipmap,
                           uint32_t dstLayer,
                           uint32_t dstMipmap) override;

    void RHIWaitUntilCompleted() override;

    // todo: need a function to collect recorded workload in this context
private:
    RHICommandContextType m_contextType;

    VulkanDevice* m_pDevice{nullptr};

    VulkanGfxState* m_pGfxState{nullptr};
    VulkanComputeState* m_pComputeState{nullptr};
};
} // namespace zen
