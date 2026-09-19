#pragma once
#include "RHICommandList.h"
#include "RHICommon.h"
#include "RHIResource.h"
#include "RHIFrameState.h"

namespace zen
{
class DynamicRHI
{
public:
    static DynamicRHI* Create(RHIAPIType type);

    virtual ~DynamicRHI() = default;

    virtual void Init() = 0;

    virtual void Destroy() = 0;

    // Final buffer/texture destruction can occur on RHI after RenderCore releases
    // its owners. Raw backends do not consume RenderCore retirement bookkeeping.
    virtual void NotifyResourceDestroyed(uint64_t resourceId) {}

    virtual void BeginFrame() = 0;

    virtual void EndFrame()
    {
        GRHIFrameState.Advance();
    }

    virtual IRHICommandContext* GetCommandContext(RHICommandContextType contextType) = 0;

    virtual IRHICommandContext* GetTransferCommandContext() = 0;

    virtual RHIAPIType GetAPIType() = 0;

    virtual NameID GetName() = 0;

    virtual DataFormat GetSupportedDepthFormat() = 0;

    // Called on the window-owning thread. Backends prepare the native surface here
    // and dispatch swapchain/backbuffer initialization to RHI.
    virtual RHIViewport* CreateViewport(void* pWindow,
                                        uint32_t width,
                                        uint32_t height,
                                        bool enableVSync) = 0;

    virtual void DestroyViewport(RHIViewport* pViewport) = 0;

    virtual RHIShader* CreateShader(const RHIShaderCreateInfo& createInfo) = 0;

    virtual void DestroyShader(RHIShader* pShader) = 0;

    virtual RHIPipeline* CreatePipeline(const RHIComputePipelineCreateInfo& createInfo) = 0;

    virtual RHIPipeline* CreatePipeline(const RHIGfxPipelineCreateInfo& createInfo) = 0;

    virtual void DestroyPipeline(RHIPipeline* pPipeline) = 0;

    virtual RHISampler* CreateSampler(const RHISamplerCreateInfo& createInfo) = 0;

    // {
    //     return m_resourceFactory->CreateSampler(samplerInfo);
    // }

    virtual void DestroySampler(RHISampler* pSampler) = 0;

    virtual RHIBindlessHandle RegisterBindlessResource(
        RHIResource* pResource,
        uint32_t slotIndex = kInvalidBindlessSlotIndex)
    {
        return {};
    }

    // Stop publishing this index to new work before unregistering. Already
    // recorded/submitted users retain the resource and prevent slot reuse.
    virtual bool UnregisterBindlessResource(RHIBindlessHandle handle)
    {
        return false;
    }

    virtual bool IsBindlessResourceRegistered(RHIBindlessHandle handle)
    {
        return false;
    }

    // Poll completion and release eligible retired registrations without waiting.
    virtual void CollectRetiredBindlessResources() {}

    virtual RHITexture* CreateTexture(const RHITextureCreateInfo& createInfo) = 0;

    virtual RHITextureView* CreateTextureView(RHITexture* pBaseTexture,
                                              const RHITextureViewCreateInfo& createInfo) = 0;

    virtual void DestroyTexture(RHITexture* pTexture) = 0;

    virtual RHIBuffer* CreateBuffer(const RHIBufferCreateInfo& createInfo) = 0;

    virtual void DestroyBuffer(RHIBuffer* pBuffer) = 0;

    //
    // virtual uint8_t* MapBuffer(BufferHandle bufferHandle) = 0;
    //
    // virtual void UnmapBuffer(BufferHandle bufferHandle) = 0;
    //
    // virtual void DestroyBuffer(BufferHandle bufferHandle) = 0;
    //
    // virtual void SetBufferTexelFormat(BufferHandle bufferHandle, DataFormat format) = 0;

    // virtual DescriptorSetHandle CreateDescriptorSet(RHIShader* shaderHandle, uint32_t setIndex) = 0;

    // virtual void DestroyDescriptorSet(RHIDescriptorSet* pDescriptorSet) = 0;

    // virtual void UpdateDescriptorSet(
    //     DescriptorSetHandle descriptorSetHandle,
    //     const std::vector<RHIShaderResourceBinding>& resourceBindings) = 0;

    // Backends prepare dependencies for the entire batch before translating any commands.
    // On failure, block submissions and leave all input lists unexecuted.
    bool PrepareCommandListDependencies(VectorView<RHICommandList*> lists)
    {
        bool result = true;
        for (RHICommandList* list : lists)
        {
            if (result)
            {
                result = list != nullptr &&
                    PrepareSubmissionDependencies(list->GetContext(),
                                                  list->GetSubmissionDependencies());
            }
        }
        return result;
    }

    virtual void FinalizeCommandLists(VectorView<RHICommandList*> cmdLists,
                                      HeapVector<RHIPlatformCommandList*>& outCommandLists) = 0;

    virtual void SubmitPlatformCommandLists(VectorView<RHIPlatformCommandList*> commandLists) = 0;

    // Consumes all queued platform lists, including rejected work. Never retries them implicitly.
    virtual RHISubmissionResult FlushAllGPUCommands() = 0;

    // Terminal backend failure, including failures discovered while polling GPU progress.
    // Raw backends are queried on their owning thread; executors publish a thread-safe snapshot.
    virtual bool AreSubmissionsBlocked() const
    {
        return false;
    }

    virtual bool IsTransferQueueSharedWithGraphics() const = 0;

    virtual bool SupportsAsyncSubmissionDependencies() const
    {
        return false;
    }

    // Attach dependencies before recording/finalizing the consumer context. Producers must
    // already be submitted; kLatestSubmitted resolves here, never to future submissions.
    // Backends without GPU waits retain a completion-wait fallback. Failure blocks the batch.
    virtual bool PrepareSubmissionDependencies(
        IRHICommandContext* context,
        VectorView<const RHISubmissionDependency> dependencies)
    {
        bool result = context != nullptr;
        for (const RHISubmissionDependency& dependency : dependencies)
        {
            if (result)
            {
                const bool validQueue = dependency.queue < RHICommandContextType::eMax;
                const uint64_t submitted =
                    validQueue ? GetLastSubmittedSerial(dependency.queue) : 0;
                const uint64_t serial =
                    dependency.serial == RHISubmissionDependency::kLatestSubmitted ?
                    submitted :
                    dependency.serial;
                result                               = validQueue && serial <= submitted;
                const RHICommandContextType consumer = context->GetContextType();
                const bool shared                    = dependency.queue == consumer ||
                    (IsTransferQueueSharedWithGraphics() &&
                     ((consumer == RHICommandContextType::eGraphics &&
                       dependency.queue == RHICommandContextType::eTransfer) ||
                      (consumer == RHICommandContextType::eTransfer &&
                       dependency.queue == RHICommandContextType::eGraphics)));
                if (result && !shared && GetLastCompletedSerial(dependency.queue) < serial)
                {
                    result = WaitForSubmission(dependency.queue, serial);
                }
            }
        }
        return result;
    }

    virtual uint64_t GetLastSubmittedSerial(RHICommandContextType contextType) const = 0;

    virtual uint64_t GetLastCompletedSerial(RHICommandContextType contextType) = 0;

    // Wait only for an already-submitted queue serial. Zero is already complete; zero timeout
    // polls. False means timeout/failure and must never be treated as permission to reuse work.
    virtual bool WaitForSubmission(RHICommandContextType contextType,
                                   uint64_t submissionSerial,
                                   uint64_t timeoutNS = UINT64_MAX) = 0;

    virtual void WaitDeviceIdle() = 0;

    virtual const RHIGPUInfo& QueryGPUInfo() const = 0;

    virtual RHITextureCopyCapabilities GetTextureCopyCapabilities(DataFormat format) const = 0;

    virtual RHIQueueCopyCapabilities GetQueueCopyCapabilities(RHICommandContextType type) const = 0;

    RHIResourceFactory* GetResourceFactory() const
    {
        return m_pResourceFactory;
    }

protected:
    RHIResourceFactory* m_pResourceFactory{nullptr};

    IRHICommandContext* m_pTransferContext{nullptr};
};

// Global instance pointer
} // namespace zen
extern zen::DynamicRHI* GDynamicRHI;
