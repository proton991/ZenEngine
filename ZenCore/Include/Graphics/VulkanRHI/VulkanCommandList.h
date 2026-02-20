#pragma once
#include "VulkanCommandList.h"
#include "VulkanQueue.h"
#include "Graphics/RHI/RHICommandList.h"
#include "Graphics/VulkanRHI/VulkanHeaders.h"
#include "Templates/HeapVector.h"
#include "Utils/Mutex.h"

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

private:
    VulkanQueue* m_pQueue{nullptr};
    FVulkanCommandBuffer* m_pCmdBuffer{nullptr};
    HeapVector<VkPipelineStageFlags> m_waitFlags;
    // DO NOT own the semaphores, only hold reference
    HeapVector<VulkanSemaphore*> m_waitSemaphores;
    HeapVector<VulkanSemaphore*> m_signalSemaphores;

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
        VulkanWorkload* pWorkload = GetWorkload();
        if (pWorkload->m_pCmdBuffer == nullptr)
        {
            SetupNewCommandBuffer();
        }

        return pWorkload->m_pCmdBuffer;
    }

    VulkanWorkload* GetWorkload()
    {
        if (m_pCurrentWorkload == nullptr)
        {
            m_pCurrentWorkload = ZEN_NEW() VulkanWorkload(m_pQueue);
        }

        return m_pCurrentWorkload;
    }

    void AddWaitSemaphore(VkPipelineStageFlags waitFlags, VulkanSemaphore* pWaitSemaphore)
    {
        AddWaitSemaphores(waitFlags, {pWaitSemaphore});
    }

    void AddWaitSemaphores(VkPipelineStageFlags waitFlags,
                           VectorView<VulkanSemaphore*> waitSemaphores)
    {
        VulkanWorkload* pCurrentWorkload = GetWorkload();
        for (uint32_t i = 0; i < waitSemaphores.size(); i++)
        {
            pCurrentWorkload->m_waitFlags.emplace_back(waitFlags);
            pCurrentWorkload->m_waitSemaphores.emplace_back(waitSemaphores[i]);
        }
    }

    void AddSignalSemaphore(VulkanSemaphore* pSignalSemaphore)
    {
        AddSignalSemaphores({pSignalSemaphore});
    }

    void AddSignalSemaphores(VectorView<VulkanSemaphore*> signalSemaphores)
    {
        VulkanWorkload* pCurrentWorkload = GetWorkload();
        for (uint32_t i = 0; i < signalSemaphores.size(); i++)
        {
            pCurrentWorkload->m_signalSemaphores.emplace_back(signalSemaphores[i]);
        }
    }

    void Finalize(HeapVector<VulkanWorkload*>& outWorkloads)
    {
        EndWorkload();
        outWorkloads.emplace_back(m_pCurrentWorkload);
    }

private:
    void SetupNewCommandBuffer();

    void StartWorkload();

    void EndWorkload();

    VulkanQueue* m_pQueue{nullptr};

    FVulkanCommandBufferPool* m_pCmdBufferPool{nullptr};

    VulkanWorkload* m_pCurrentWorkload{nullptr};
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
    HeapVector<VulkanDescriptorSet*> m_descriptorSets{nullptr};

    HeapVector<VulkanBuffer*> m_vertexBuffers;
    HeapVector<uint32_t> m_vertexBufferOffsets;

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

    void RHIBindVertexBuffer(RHIBuffer* pBuffer, uint64_t offset) override;

    void RHIDrawIndexed(RHIBuffer* pIndexBuffer,
                        uint32_t indexCount,
                        uint32_t instanceCount,
                        uint32_t firstIndex,
                        int32_t vertexOffset,
                        uint32_t firstInstance) override;

    void RHIDrawIndexedIndirect(RHIBuffer* pIndirectBuffer,
                                RHIBuffer* pIndexBuffer,
                                uint32_t offset,
                                uint32_t drawCount,
                                uint32_t stride) override;

    void RHIDispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override;

    void RHIDispatchIndirect(RHIBuffer* pIndirectBuffer, uint32_t offset) override;

    void RHIAddTransitions(BitField<RHIPipelineStageBits> srcStages,
                           BitField<RHIPipelineStageBits> dstStages,
                           const HeapVector<RHIMemoryTransition>& memoryTransitions,
                           const HeapVector<RHIBufferTransition>& bufferTransitions,
                           const HeapVector<RHITextureTransition>& textureTransitions) override;

    void RHICopyBufferToTexture(RHIBuffer* pSrcBuffer,
                                RHITexture* pDstTexture,
                                uint32_t numRegions,
                                RHIBufferTextureCopyRegion* pRegions) override;

    void RHIGenTextureMipmaps(RHITexture* pTexture) override;

    void RHIAddTextureTransition(RHITexture* pTexture, RHITextureLayout newLayout) override;

    // todo: need a function to collect recorded workload in this context
private:
    RHICommandContextType m_contextType;

    VulkanDevice* m_pDevice{nullptr};

    VulkanGfxState* m_pGfxState{nullptr};
};
} // namespace zen