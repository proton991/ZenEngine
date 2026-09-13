#pragma once
#include "DynamicRHI.h"
#include "RHIThread.h"
#include "Templates/SmallVector.h"
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>

namespace zen
{
struct RHIBatchResult
{
    RHISubmissionResult submission{RHISubmissionResult::eRejected};
    SmallVector<uint64_t, 3> serials{0, 0, 0};
    bool presented{false};
    bool needsRecreation{false};
    double executionCPUUs{0};
    double queueWaitUs{0};
    std::string error;
};

class RHISubmissionTicket
{
public:
    RHISubmissionTicket() = default;
    RHISubmissionTicket(std::shared_future<RHIBatchResult> result,
                        std::shared_ptr<RHIThreadEvent> completion);
    bool IsValid() const;
    bool IsReady() const;
    RHIBatchResult Wait() const;

private:
    std::shared_future<RHIBatchResult> m_result;
    std::shared_ptr<RHIThreadEvent> m_completion;
};

// Both the command arena and referenced resources survive until GPU retirement.
// This conservative first implementation can later recycle the arena after translation.
struct RHICommandBatch
{
    RHICommandListPtr commands;
    RHICommandListPtr presentCommands;
    RHIResourceReferences resources;
    RHIViewport* viewport{nullptr};
    std::promise<RHIBatchResult> completion;
    std::shared_ptr<RHIThreadEvent> completionEvent{std::make_shared<RHIThreadEvent>()};
    std::chrono::steady_clock::time_point queuedAt;
    RHIBatchResult result;
    bool executionFinished{false};

    RHICommandBatch()                                  = default;
    RHICommandBatch(const RHICommandBatch&)            = delete;
    RHICommandBatch& operator=(const RHICommandBatch&) = delete;
    RHICommandBatch(RHICommandBatch&&)                 = default;
    RHICommandBatch& operator=(RHICommandBatch&&)      = default;
};

struct RHIThreadMetrics
{
    uint64_t submittedBatches{0};
    uint64_t completedBatches{0};
    uint64_t executionCPUUs{0};
    uint64_t queueWaitUs{0};
    uint64_t pendingBatches{0};
    uint64_t peakPendingBatches{0};
};

// DynamicRHI facade: RenderCore keeps its API, while mutable backend operations
// are marshalled to one executor. Raw backend tests can still run without it.
class RHICommandListExecutor final : public DynamicRHI
{
public:
    RHICommandListExecutor(DynamicRHI* backend, RHIExecutionMode mode);
    ~RHICommandListExecutor() override;
    RHICommandListExecutor(const RHICommandListExecutor&)            = delete;
    RHICommandListExecutor& operator=(const RHICommandListExecutor&) = delete;

    RHISubmissionTicket SubmitFrame(RHICommandList& commands, RHIViewport* viewport);
    RHISubmissionResult SubmitBatch(VectorView<RHICommandList*> lists);
    // Request one coalesced, nonblocking GPU progress/retirement sweep on RHI.
    // Completed serial getters remain snapshots; poll again if work is still in flight.
    void PollGPUProgress();
    void FlushRHIThread();
    bool AreSubmissionsBlocked() const override;
    RHIThreadMetrics GetThreadMetrics() const;

    void NotifyResourceDestroyed(uint64_t resourceId) override;
    void DrainDestroyedResourceIds(HeapVector<uint64_t>& resourceIds);

    void Destroy() override;
    void BeginFrame() override;
    void EndFrame() override;
    RHISubmissionResult FlushAllGPUCommands() override;
    void WaitDeviceIdle() override;
    bool WaitForSubmission(RHICommandContextType type,
                           uint64_t serial,
                           uint64_t timeoutNS = UINT64_MAX) override;
    uint64_t GetLastSubmittedSerial(RHICommandContextType type) const override;
    uint64_t GetLastCompletedSerial(RHICommandContextType type) override;
    RHIAPIType GetAPIType() override;
    NameID GetName() override;
    DataFormat GetSupportedDepthFormat() override;
    bool IsTransferQueueSharedWithGraphics() const override;
    const RHIGPUInfo& QueryGPUInfo() const override;
    RHIQueueCopyCapabilities GetQueueCopyCapabilities(RHICommandContextType type) const override;
    void Init() override;
    void DestroyViewport(RHIViewport* viewport) override;
    RHIViewport* CreateViewport(void* window, uint32_t width, uint32_t height, bool vsync) override;
    RHIShader* CreateShader(const RHIShaderCreateInfo& info) override;
    void DestroyShader(RHIShader* shader) override;
    RHIPipeline* CreatePipeline(const RHIComputePipelineCreateInfo& info) override;
    RHIPipeline* CreatePipeline(const RHIGfxPipelineCreateInfo& info) override;
    void DestroyPipeline(RHIPipeline* pipeline) override;
    RHISampler* CreateSampler(const RHISamplerCreateInfo& info) override;
    void DestroySampler(RHISampler* sampler) override;
    RHIBindlessHandle RegisterBindlessResource(RHIResource* resource, uint32_t slot) override;
    bool UnregisterBindlessResource(RHIBindlessHandle handle) override;
    bool IsBindlessResourceRegistered(RHIBindlessHandle handle) override;
    void CollectRetiredBindlessResources() override;
    RHITexture* CreateTexture(const RHITextureCreateInfo& info) override;
    RHITextureView* CreateTextureView(RHITexture* texture,
                                      const RHITextureViewCreateInfo& info) override;
    void DestroyTexture(RHITexture* texture) override;
    RHIBuffer* CreateBuffer(const RHIBufferCreateInfo& info) override;
    void DestroyBuffer(RHIBuffer* buffer) override;
    IRHICommandContext* GetCommandContext(RHICommandContextType type) override;
    IRHICommandContext* GetTransferCommandContext() override;
    void FinalizeCommandLists(VectorView<RHICommandList*> lists,
                              HeapVector<RHIPlatformCommandList*>& output) override;
    void SubmitPlatformCommandLists(VectorView<RHIPlatformCommandList*> lists) override;
    RHITextureCopyCapabilities GetTextureCopyCapabilities(DataFormat format) const override;

private:
    void ExecuteFrame(const std::shared_ptr<RHICommandBatch>& batch);
    RHISubmissionResult ExecuteBatch(VectorView<RHICommandList*> lists);
    void ExecuteBeginFrame();
    void ExecuteWaitIdle();
    void ExecuteDestroy();
    void DeleteBackend();
    void PublishProgress();
    void PublishSubmissionStatus();
    void RefreshGPUProgress();
    void ExecutePollGPUProgress();
    void CollectCompletedBatches(bool force = false);
    RHICommandListPtr AcquireRecordingCommandList();
    RHICommandListPtr AcquirePresentCommandList();
    void RecycleCommandLists(RHICommandBatch& batch);

    DynamicRHI* m_backend;
    RHIExecutionMode m_mode;
    std::thread::id m_renderThread;
    std::atomic<bool> m_blocked{false};
    std::atomic<bool> m_progressPollPending{false};
    bool m_destroyed{false};
    RHIGPUInfo m_gpuInfo;
    RHIAPIType m_apiType;
    NameID m_name;
    DataFormat m_depthFormat;
    bool m_sharedTransfer;
    SmallVector<RHIQueueCopyCapabilities, 3> m_queueCapabilities;
    // SmallVector relocation requires movable elements; atomic counters need fixed storage.
    std::array<std::atomic<uint64_t>, 3> m_submitted{};
    std::array<std::atomic<uint64_t>, 3> m_completed{};
    HeapVector<std::shared_ptr<RHICommandBatch>> m_retired;
    // Limit idle storage after a burst. In-flight batches always retain their own lists.
    static constexpr size_t kMaxRecycledCommandLists = RHIFrameState::kMaxFramesInFlight;
    std::mutex m_recordingCommandListMutex;
    HeapVector<RHICommandListPtr> m_recordingCommandLists;
    // Presentation lists and their contexts stay on RHI until executor destruction.
    HeapVector<RHICommandListPtr> m_presentCommandLists;
    std::mutex m_destroyedResourceMutex;
    HeapVector<uint64_t> m_destroyedResourceIds;
    std::atomic<uint64_t> m_submittedBatches{0};
    std::atomic<uint64_t> m_completedBatches{0};
    std::atomic<uint64_t> m_executionCPUUs{0};
    std::atomic<uint64_t> m_queueWaitUs{0};
    std::atomic<uint64_t> m_pendingBatches{0};
    std::atomic<uint64_t> m_peakPendingBatches{0};
};
} // namespace zen
