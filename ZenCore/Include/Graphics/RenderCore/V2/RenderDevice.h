#pragma once
#include "Graphics/RHI/RHICommon.h"
#include "Graphics/RHI/RHIFrameState.h"
#include "Graphics/RHI/RHIResource.h"
#include "Graphics/RenderCore/V2/RenderFrameState.h"
#include "Graphics/RenderCore/V2/PipelineCacheMetrics.h"
#include "Graphics/RenderCore/V2/RenderGraph/RDGPassCompiler.h"
#include "Graphics/RenderCore/V2/StagingUploadQueue.h"
#include "Templates/HashMap.h"
#include "Templates/HeapVector.h"
#include "Templates/LRUCache.h"
#include "Templates/SmallVector.h"
#include "Templates/ObjectPool.h"
#include "Templates/Queue.h"
#include "Templates/VectorView.h"
#include "Graphics/RHI/RHICommandList.h"
#include "Graphics/RHI/RHIDebug.h"
#include "RenderGraph/RenderGraph.h"
#include "RenderCoreDefs.h"
#include "Utils/UniquePtr.h"
#include "Graphics/RHI/RHICommandListExecutor.h"
#include "RenderConfig.h"
#include "RenderSubmissionHistory.h"

namespace zen::sg
{
class Scene;
}

namespace zen::rc
{
class RenderDevice;
class RenderGraph;
class RendererServer;
class TextureManager;
class SkyboxRenderer;

template <RHICommandContextType Context> struct CommandListPoolPolicy
{
    static RHICommandList* Create()
    {
        return RHICommandList::Create(GDynamicRHI->GetCommandContext(Context));
    }

    static void Reset(RHICommandList* list)
    {
        list->Reset();
    }

    static void Destroy(RHICommandList* list)
    {
        list->Reset();
        ZEN_DELETE(list);
    }
};

struct RenderFrame
{
    HeapVector<RHIBuffer*> buffersPendingFree;
    HeapVector<RHITexture*> texturesPendingFree;
    HeapVector<RHIPipeline*> pipelinesPendingFree;
    HeapVector<RHIResource*> resourcesPendingRelease;
    RHIRetirementRequirement retirement;
    RHISubmissionTicket submission;
};

struct RenderDeviceFeatures
{
    bool geometryShader{false};
};

// todo: some resources may be freed or recycled, track these and free them before frame begins
class RenderDevice
{
public:
    explicit RenderDevice(
        RHIAPIType APIType,
        uint32_t numFrames,
        RHIExecutionMode executionMode    = RenderConfig::GetInstance().rhiExecutionMode,
        AsyncComputeMode asyncComputeMode = RenderConfig::GetInstance().asyncComputeMode);

    void Init(RHIViewport* pMainViewport);

    void Destroy();

    // Executes the RDG on the render thread. In threaded mode, success means the RHI executor
    // accepted the recorded frame; poll/flush confirms native submission.
    bool ExecuteRenderGraph(RHIViewport* pViewport);

    bool PollFrameSubmissions(bool wait = false);

    void FlushRHIThread();

    RHIThreadMetrics GetRHIThreadMetrics() const;

    const RHIQueueCapabilities& GetQueueCapabilities() const
    {
        return m_queueCapabilities;
    }

    AsyncComputeStatus GetAsyncComputeStatus() const
    {
        return m_asyncComputeStatus;
    }

    RDGAsyncComputeEligibility ResolveAsyncComputeEligibility(const RenderGraph& graph,
                                                              const RDGPassNode& node) const;

    bool ExecuteRenderGraph(RenderGraph& rdg);

    // The caller owns the initial resource reference; no persistent device owner.
    RHIBuffer* CreateBuffer(const RHIBufferCreateInfo& info);
    RHITexture* CreateTexture(const RHITextureCreateInfo& info);
    RHICompletionSet GetSubmittedCompletion() const;
    RHICompletionSet GetCompletedCompletion() const;
    RHIRetirementRequirement CaptureResourceRetirement() const;
    bool IsResourceRetired(const RHIRetirementRequirement& requirement) const;

    // Copy declarations precede graph compilation/device binding. Use the active
    // RenderDevice's RHI facade for capability validation at declaration time.
    static RHIQueueCopyCapabilities GetQueueCopyCapabilities(RHICommandContextType queue);
    static RHITextureCopyCapabilities GetTextureCopyCapabilities(DataFormat format);

    bool AreSubmissionsBlocked() const
    {
        return m_submissionBlocked ||
            (m_pRHIExecutor != nullptr && m_pRHIExecutor->AreSubmissionsBlocked());
    }

    void InvalidateRDGPassCompilerForResize();

    RenderGraph* GetCurrentFrameRDG() const
    {
        return m_frameRDG.Get();
    }

    void InvalidateExternalTextureState(RHITexture* pTexture);

    void InvalidateExternalBufferState(RHIBuffer* pBuffer);

    RHIRenderingLayout* AcquireRenderingLayout();

    void ReleaseRenderingLayout(RHIRenderingLayout* pLayout);

    void DestroyRenderingLayout(RHIRenderingLayout* pLayout);

    RHITexture* CreateTextureColorRT(const TextureFormat& texFormat,
                                     TextureUsageHint usageHint,
                                     NameID texName);

    RHITexture* CreateTextureDepthStencilRT(const TextureFormat& texFormat,
                                            TextureUsageHint usageHint,
                                            NameID texName);

    RHITexture* CreateTextureStorage(const TextureFormat& texFormat,
                                     TextureUsageHint usageHint,
                                     NameID texName);

    RHITexture* CreateTextureSampled(const TextureFormat& texFormat,
                                     TextureUsageHint usageHint,
                                     NameID texName);

    RHITexture* CreateTextureDummy(const TextureFormat& texFormat,
                                   TextureUsageHint usageHint,
                                   NameID texName);

    RHITextureView* CreateTextureView(RHITexture* pTexture,
                                      const TextureViewFormat& viewFormat,
                                      NameID texViewName);

    // RHITexture* GetTextureRDFromHandle(const RHITexture* handle);

    void DestroyTexture(RHITexture* pTexture);

    // RHITexture* CreateTexture(const TextureInfo& textureInfo);
    //
    // RHITexture* CreateTextureProxy(const RHITexture* baseTexture,
    //                                       const TextureProxyInfo& proxyInfo);

    // RHITexture* GetBaseTextureForProxy(const RHITexture* handle) const;

    // bool IsProxyTexture(const RHITexture* handle) const;

    RHIBuffer* CreateVertexBuffer(uint32_t dataSize, const uint8_t* pData);

    RHIBuffer* CreateIndexBuffer(uint32_t dataSize, const uint8_t* pData);

    RHIBuffer* CreateUniformBuffer(uint32_t dataSize, const uint8_t* pData, NameID bufferName);

    RHIBuffer* CreateStorageBuffer(uint32_t dataSize, const uint8_t* pData, NameID bufferName);

    RHIBuffer* CreateIndirectBuffer(uint32_t dataSize, const uint8_t* pData, NameID bufferName);

    void UpdateBuffer(RHIBuffer* pBufferHandle,
                      uint32_t dataSize,
                      const uint8_t* pData,
                      uint32_t offset = 0);

    void DestroyBuffer(RHIBuffer* pBufferHandle);

    bool ResolveStagingFlushAction(StagingFlushAction action,
                                   StagingBufferManager* manager = nullptr);

    // RenderPassHandle GetOrCreateRenderPass(const RHIRenderPassLayout& layout);

    // RHIPipeline* GetOrCreateGfxPipeline(RHIGfxPipelineStates& PSO,
    //                                     RHIShader* shader,
    //                                     const RenderPassHandle& renderPass,
    //                                     const HashMap<uint32_t, int>& specializationConstants);

    RHIPipeline* GetOrCreateGfxPipeline(const RHIGfxPipelineStates& PSO,
                                        RHIShader* pShader,
                                        const RHIRenderingLayout* pRenderingLayout,
                                        const HashMap<uint32_t, int>& specializationConstants);

    RHIPipeline* GetOrCreateComputePipeline(RHIShader* pShader);

    const PipelineCacheMetrics& GetPipelineCacheMetrics() const
    {
        return m_pipelineMetrics;
    }

    RDGPassCompiler& GetRDGPassCompiler(RHIPipeline* pPipeline)
    {
        return m_rdgPassCompiler;
    }

    RDGMetrics& GetRDGMetrics()
    {
        return m_rdgExecutor.GetMetrics();
    }

    void DeferDestroyPipeline(RHIPipeline* pPipeline);

    // Retire one already-owned reference after submitted work on both queues completes.
    void DeferReleaseResource(RHIResource* resource);

    // Nonblocking retirement sweep, also usable when no new frame is being started.
    void CollectCompletedResources();

    RHIViewport* CreateViewport(void* pWindow,
                                uint32_t width,
                                uint32_t height,
                                bool enableVSync = true);

    void ResizeViewport(RHIViewport* pViewport, uint32_t width, uint32_t height);

    // RHITexture* LoadTexture2D(const std::string& file, bool requireMipmap = false);

    RHITexture* LoadTexture2D(const std::string& file, bool requireMipmap = false);

    void LoadSceneTextures(const sg::Scene* pScene, HeapVector<RHITexture*>& outTextures);

    void LoadTextureEnv(const std::string& file, EnvTexture* pTexture);

    RHISampler* CreateSampler(const RHISamplerCreateInfo& samplerInfo);

    // RHITextureSubResourceRange GetTextureSubResourceRange(RHITexture* handle);

    RHIDebug* GetRHIDebug() const
    {
        return m_pRHIDebug;
    }

    // RHICommandList* GetCurrentCmdList() const
    // {
    //     return m_frames[m_currentFrame].pGfxCmdList;
    // }

    // RHICommandList* GetCurrentDrawCmdList() const
    // {
    //     return m_frames[m_currentFrame].drawCmdList;
    // }

    RendererServer* GetRendererServer() const
    {
        return m_pRendererServer;
    }

    void ProcessViewportResize(uint32_t width, uint32_t height);

    void WaitForPreviousFrames();

    void NextFrame();

    void WaitForIdle() const
    {
        GDynamicRHI->WaitDeviceIdle();
    }

    const RHIGPUInfo& GetGPUInfo() const
    {
        return GDynamicRHI->QueryGPUInfo();
    }

private:
    struct PendingFrame
    {
        RHISubmissionTicket ticket;
        // Selected from GRenderFrameState at dispatch; this frame storage never moves.
        RenderFrame* frame{nullptr};
        RHIViewport* viewport{nullptr};
        ResourceStateTracker scheduledState;
        HeapVector<RDGDeferredExtraction> extractions;
        NameID graphName;
        uint32_t computePassCount{0};
    };

    // Executes the frame RDG here, with native submission deferred to the RHI thread.
    bool ExecuteFrameGraph(RHIViewport* viewport);

    RHISubmissionResult SubmitRecordedGraph(RenderGraph& graph, RHICommandList& commands);
    bool ExecuteScheduledGraph(RDGExecutor::ExecutionPlan& plan,
                               RHIViewport* viewport = nullptr,
                               PendingFrame* pending = nullptr);
    RHISubmissionResult SubmitRecordedGroups(RenderGraph& graph,
                                             const RDGSchedule& schedule,
                                             RenderSubmissionUpdate& update,
                                             VectorView<RHICommandList*> lists,
                                             RHIViewport* viewport,
                                             PendingFrame* pending);

    bool PrepareGraphSubmission(const RenderGraph& graph,
                                RHICommandList& commands,
                                RenderSubmissionUpdate& update);

    bool PrepareScheduledSubmissionHistory(const RenderGraph& graph,
                                           const RDGSchedule& schedule,
                                           RenderSubmissionUpdate& update,
                                           bool serializeReads = false);

    void LogTransferSubmission(const RenderGraph& graph,
                               RHICommandContextType queue,
                               uint64_t serial);
    void LogAsyncComputeSubmission(NameID graphName,
                                   uint32_t computePassCount,
                                   const RHIBatchResult& result);

    // Submits recorded commands to the RHI executor and retains its completion ticket.
    RHISubmissionResult SubmitRecordedFrame(RenderGraph& graph,
                                            RHICommandList& commands,
                                            RHIViewport* viewport,
                                            PendingFrame& pending);
    void CompleteFrame(PendingFrame& pending, const RHIBatchResult& result);
    void CollectDestroyedResourceHistory();
    void ProcessDeferredViewportResize();

    uint32_t GetCurrentFrameSlot() const
    {
        return ToIndex(GRenderFrameState.GetFrameSlot());
    }

    void BeginFrame();

    void EndFrame();

    RHISubmissionResult SubmitImmediateTransferCmdList();

    void AcquireGraphicsCmdLists(size_t numCmdLists, HeapVector<RHICommandList*>& outCmdLists);

    void AcquireScheduledCmdLists(const RDGSchedule& schedule, HeapVector<RHICommandList*>& lists);
    void ReleaseScheduledCmdLists(VectorView<RHICommandList*> lists);

    RHISubmissionResult SubmitCommandLists(VectorView<RHICommandList*> cmdLists);

    void ProcessPendingFreeResources(RenderFrameSlot frameSlot, bool ignoreCompletionGate = false);

    void StampOutgoingFrameSerials();

    bool IsViewportResource(const RHIResource* resource) const;

    void InitializeBufferData(RHIBuffer* buffer, uint32_t dataSize, const uint8_t* data);

    void UpdateBufferInternal(RHIBuffer* pBufferHandle,
                              uint32_t offset,
                              uint32_t dataSize,
                              const uint8_t* pData);

    // void UpdateTextureOneTime(RHITexture* pTextureHandle,
    //                           const Vec3i& textureSize,
    //                           uint32_t dataSize,
    //                           const uint8_t* pData);

    // void UpdateTextureBatch(RHITexture* pTextureHandle,
    //                         const Vec3i& textureSize,
    //                         const uint8_t* pData);

    void DestroyViewport(RHIViewport* pViewport);

    // Canonical field values, never raw struct padding or a hash used as identity.
    // Inline storage covers all graphics state plus common specialization overrides.
    struct PipelineKey
    {
        SmallVector<uint32_t, 64 + 20 * MAX_NUM_COLOR_ATTACHMENTS> words;
        size_t hash{0};

        void AddWord(uint32_t value);

        template <typename T> void Add(T value);

        void AddStencil(const RHIStencilOpState& op);

        void AddAttachment(const RHIRenderTarget& target, bool dynamicRendering);

        bool operator==(const PipelineKey& other) const
        {
            return words.size() == other.words.size() &&
                std::equal(words.begin(), words.end(), other.words.begin());
        }
    };
    struct PipelineKeyHasher
    {
        size_t operator()(const PipelineKey& key) const
        {
            return key.hash;
        }
    };
    using PipelineCache = LRUCache<PipelineKey, RHIPipeline*, PipelineKeyHasher>;

    static PipelineKey MakePipelineKey(RHIShader* shader,
                                       const RHIGfxPipelineStates* states      = nullptr,
                                       const RHIRenderingLayout* layout        = nullptr,
                                       const HashMap<uint32_t, int>& constants = {},
                                       bool dynamicRendering                   = false);

    friend struct PipelineCacheTestAccess;
    friend struct RDGSubmissionTestAccess;
    friend struct RDGExecutionPlanTestAccess;

    // The compiler uses its own executor's timing option, including standalone executors.
    RHIPipeline* GetOrCreateGfxPipeline(const RHIGfxPipelineStates& states,
                                        RHIShader* shader,
                                        const RHIRenderingLayout* layout,
                                        const HashMap<uint32_t, int>& constants,
                                        bool timed);

    RHIPipeline* GetOrCreateComputePipeline(RHIShader* shader, bool timed);

    static size_t CalcSamplerHash(const RHISamplerCreateInfo& info);

    size_t PadUniformBufferSize(size_t originalSize);

    size_t PadStorageBufferSize(size_t originalSize);

    const RHIAPIType m_APIType;
    const uint32_t m_numFrames;
    const RHIExecutionMode m_executionMode;
    const AsyncComputeMode m_asyncComputeMode;
    RHIQueueCapabilities m_queueCapabilities;
    AsyncComputeStatus m_asyncComputeStatus{AsyncComputeStatus::eDisabled};
    RHICommandListExecutor* m_pRHIExecutor{nullptr};
    HeapVector<PendingFrame> m_pendingFrames;
    ResourceStateTracker m_confirmedResourceState;
    // Render-thread submission history; the graph never queries backend queue progress.
    RenderSubmissionHistory m_submissionHistory;
    HeapVector<uint64_t> m_destroyedResourceIds;
    RHIViewport* m_pRecreateViewport{nullptr};

    HeapVector<RenderFrame> m_frames;

    // DynamicRHI* GDynamicRHI{nullptr};
    RHIDebug* m_pRHIDebug{nullptr};

    ObjectPool<RHICommandList, CommandListPoolPolicy<RHICommandContextType::eGraphics>>
        m_graphicsCmdListPool;
    ObjectPool<RHICommandList, CommandListPoolPolicy<RHICommandContextType::eAsyncCompute>>
        m_computeCmdListPool;
    ObjectPool<RHICommandList, CommandListPoolPolicy<RHICommandContextType::eTransfer>>
        m_transferCmdListPool;
    RHICommandList* m_pImmediateTransferCmdList{nullptr};
    bool m_loggedTransferSubmission{false};
    bool m_loggedGraphicsTransferSubmission{false};
    bool m_loggedAsyncComputeSubmission{false};

    StagingBufferManager m_stagingBufferManager;

    StagingUploadQueue* m_pUploadQueue{nullptr};

    RDGExecutor m_rdgExecutor;
    RDGPassCompiler m_rdgPassCompiler;
    UniquePtr<RenderGraph> m_frameRDG;

    RendererServer* m_pRendererServer{nullptr};
    TextureManager* m_pTextureManager{nullptr};

    DeletionQueue m_deletionQueue;

    // HashMap<size_t, RenderPassHandle> m_renderPassCache;
    static constexpr size_t kPipelineCacheCapacity = 256;
    PipelineCache m_pipelineCache;
    PipelineCacheMetrics m_pipelineMetrics;

    HashMap<size_t, RHISampler*> m_samplerCache;

    // HashMap<RHITexture*, RHITexture*> m_textureMap;
    HeapVector<RHIBuffer*> m_buffers;

    HeapVector<RHIViewport*> m_viewports;
    RHIViewport* m_pMainViewport{nullptr};

    HeapVector<RHIRenderingLayout*> m_renderingLayoutPool;
    HeapVector<GraphicsPass*> m_gfxPassPool;

    bool m_resolvingStagingFlush{false};
    bool m_frameActive{false};
    bool m_frameWaitFailed{false};
    bool m_submissionBlocked{false};

    friend class RDGPassCompiler;
};
} // namespace zen::rc
