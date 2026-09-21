#pragma once
#include <mutex>
#include "DynamicRHI.h"
#include "RHIThread.h"
#include "Templates/SmallVector.h"
#include "Utils/UniquePtr.h"
#include <atomic>
#include <chrono>
#include <future>
#include "Utils/RefCountPtr.h"

namespace zen
{
enum class RHISubmissionPointStatus : uint8_t
{
    eInvalid,
    ePending,
    eAccepted,
    eFailed
};

// Owned native-acceptance results. Queue assignments never change after construction.
// RenderCore can retain a point while the ordered RHI worker fills its exact serial.
class RHISubmissionState final : public RefCounted
{
public:
    explicit RHISubmissionState(VectorView<const RHICommandContextType> queues);
    bool IsQueued() const;
    bool Matches(uint32_t group, RHICommandContextType queue) const;
    size_t GetGroupCount() const;
    // Successful CPU processing of every group; does not establish GPU completion.
    bool IsSubmissionFinished() const;
    RHISubmissionPointStatus Resolve(uint32_t group, RHISubmissionDependency& submission) const;

private:
    friend class RHICommandListExecutor;
    friend struct RHISubmissionStateTestAccess;

    // Only the executor publishes handoff, native acceptance, and final status.
    bool Queue();
    bool Accept(uint32_t group, RHISubmissionDependency submission);
    bool FinishSubmission();
    void Fail();

    struct Group
    {
        RHISubmissionDependency submission;
        bool accepted{false};
    };
    HeapVector<Group> m_groups;
    mutable std::mutex m_mutex;
    bool m_queued{false};
    bool m_failed{false};
    bool m_submissionFinished{false};
};

struct RHISubmissionPoint
{
    RHICommandContextType queue{RHICommandContextType::eGraphics};
    uint64_t serial{0};
    RefCountPtr<RHISubmissionState> state;
    uint32_t group{UINT32_MAX};

    bool IsValid() const;
    bool operator==(const RHISubmissionPoint& other) const;
    RHISubmissionPointStatus Resolve(RHISubmissionDependency& submission) const;
};

// Borrowed recording inputs. Handoff validates the complete schedule before detaching any list.
struct RHISubmissionGroup
{
    RHICommandList* commands{nullptr};
    HeapVector<RHISubmissionPoint> predecessors;
};

struct RHISubmissionGroupResult
{
    RHISubmissionResult submission{RHISubmissionResult::eRejected};
    RHISubmissionDependency accepted;
};

struct RHIBatchResult
{
    RHISubmissionResult submission{RHISubmissionResult::eRejected};
    RHICompletionSet requiredSerials;
    HeapVector<RHISubmissionGroupResult> groups;
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
                        RefCountPtr<RHIThreadEvent> completion);
    bool IsValid() const;
    bool IsReady() const;
    // Waits for CPU submission processing, not GPU completion. The result is
    // borrowed from this ticket; keep a ticket alive while using the reference.
    const RHIBatchResult& Wait() const;

private:
    std::shared_future<RHIBatchResult> m_result;
    RefCountPtr<RHIThreadEvent> m_completion;
};

// Both the command arena and referenced resources survive until GPU retirement.
// This conservative first implementation can later recycle the arena after translation.
struct RHICommandBatch : RefCounted
{
    struct Group
    {
        RHICommandListPtr commands;
        HeapVector<RHISubmissionPoint> predecessors;
    };
    HeapVector<Group> groups;
    RHICommandListPtr presentCommands;
    RHIResourceReferences resources;
    RefCountPtr<RHISubmissionState> submissionState;
    RHIViewport* viewport{nullptr};
    std::promise<RHIBatchResult> completion;
    RefCountPtr<RHIThreadEvent> completionEvent{MakeRefCountPtr<RHIThreadEvent>()};
    std::chrono::steady_clock::time_point queuedAt;
    RHIBatchResult result;
    bool executionFinished{false};
    bool endFrame{true};

    RHICommandBatch()                                  = default;
    RHICommandBatch(const RHICommandBatch&)            = delete;
    RHICommandBatch& operator=(const RHICommandBatch&) = delete;
    RHICommandBatch(RHICommandBatch&&)                 = delete;
    RHICommandBatch& operator=(RHICommandBatch&&)      = delete;
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

    RHISubmissionTicket SubmitFrame(RHICommandList& commands,
                                    RHIViewport* viewport,
                                    RefCountPtr<RHISubmissionState> state             = {},
                                    VectorView<const RHISubmissionPoint> predecessors = {});
    RHISubmissionTicket SubmitFrame(VectorView<const RHISubmissionGroup> groups,
                                    RHIViewport* viewport,
                                    RefCountPtr<RHISubmissionState> state = {});
    // Synchronous CPU handoff for standalone graphs; does not end the native frame.
    RHIBatchResult SubmitGroups(VectorView<const RHISubmissionGroup> groups,
                                RefCountPtr<RHISubmissionState> state);
    RHISubmissionResult SubmitBatch(VectorView<RHICommandList*> lists);
    // Request one coalesced, nonblocking GPU progress/retirement sweep on RHI.
    // Cached getters do not refresh progress; poll again if work is still in flight.
    void PollGPUProgress();
    // Published values only, in both execution modes. No backend calls or worker waits.
    RHICompletionSet GetCachedSubmittedSerials() const;
    RHICompletionSet GetCachedCompletedSerials() const;
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
    bool WaitForCompletion(RHICommandContextType type,
                           uint64_t serial,
                           uint64_t timeoutNS = UINT64_MAX) override;
    uint64_t GetLastSubmittedSerial(RHICommandContextType type) const override;
    // Polls the backend inline; threaded mode reads published progress without a worker wait.
    uint64_t QueryLastCompletedSerial(RHICommandContextType type) override;
    RHIAPIType GetAPIType() override;
    NameID GetName() override;
    DataFormat GetSupportedDepthFormat() override;
    RHIQueueCapabilities GetQueueCapabilities() const override;
    bool PrepareSubmissionDependencies(
        IRHICommandContext* context,
        VectorView<const RHISubmissionDependency> dependencies) override;
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
    RHISubmissionTicket QueueBatch(VectorView<const RHISubmissionGroup> groups,
                                   RHIViewport* viewport,
                                   RefCountPtr<RHISubmissionState> state,
                                   bool endFrame);
    bool ValidateGroups(VectorView<const RHISubmissionGroup> groups,
                        const RHISubmissionState& state) const;
    bool ResolvePredecessors(RHICommandList& commands, VectorView<const RHISubmissionPoint> points);
    bool ExecuteGroups(RHICommandBatch& batch);
    struct QueueProgress
    {
        std::atomic<uint64_t> submitted{0};
        std::atomic<uint64_t> completed{0};
    };

    void ExecuteFrame(const RefCountPtr<RHICommandBatch>& batch);
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
    RHICommandListPtr AcquireRecordingCommandList(RHICommandContextType queue);
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
    const RHIQueueCapabilities m_submissionQueueCapabilities;
    SmallVector<RHIQueueCopyCapabilities, RHICompletionSet::kQueueCount> m_queueCapabilities;
    // Populate before starting the worker; owned atomics retain stable addresses.
    SmallVector<UniquePtr<QueueProgress>, RHICompletionSet::kQueueCount> m_queueProgress;
    HeapVector<RefCountPtr<RHICommandBatch>> m_retired;
    // Limit idle storage after a burst. In-flight batches always retain their own lists.
    static constexpr size_t kMaxRecycledCommandLists = RHIFrameState::kMaxFramesInFlight;
    std::mutex m_recordingCommandListMutex;
    SmallVector<HeapVector<RHICommandListPtr>, RHICompletionSet::kQueueCount>
        m_recordingCommandLists =
            SmallVector<HeapVector<RHICommandListPtr>, RHICompletionSet::kQueueCount>(
                RHICompletionSet::kQueueCount);
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
