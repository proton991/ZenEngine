#pragma once
#include <unordered_set>
#include "Graphics/RHI/RHICommon.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Templates/BitField.h"
#include "Templates/FlatHashMap.h"
#include "Templates/HashMap.h"
#include "Templates/NameID.h"
#include "Utils/Errors.h"
#include "Templates/ArenaVector.h"
#include "Templates/HeapVector.h"
#include "Memory/PagedAllocator.h"
#include "Memory/PoolAllocator.h"
#include "../RenderCoreDefs.h"
#include "RDGDefs.h"
#include "RDGSchedule.h"
#include "RDGResourceManager.h"
#include "RDGPassCompiler.h"
#include "RDGMetrics.h"
#include <array>

namespace zen
{
class RHICommandList;
}

namespace zen::rc
{
template <typename T> using RDGVector = ArenaVector<T, PoolAllocator<LinearAllocator>>;

struct RDGWriterVisibility
{
    bool hasWriter{false};
    BitField<RHIAccessFlagBits> access;
    BitField<RHIPipelineStageFlagBits> stages;

    // Visibility is a set of (access, stage) pairs, not two independent mask unions.
    std::array<int64_t, 17> visibleStages{};
};

struct RDGTextureResourceState
{
    RHIAccessMode accessMode{RHIAccessMode::eNone};
    RHITextureUsage usage{RHITextureUsage::eNone};
    BitField<RHIPipelineStageFlagBits> pipelineStages{RHIPipelineStageFlagBits::eTopOfPipe};
    RDGWriterVisibility writer;
};

struct RDGBufferResourceState
{
    RHIAccessMode accessMode{RHIAccessMode::eNone};
    BitField<RHIBufferUsageFlagBits> usage{};
    BitField<RHIPipelineStageFlagBits> pipelineStages{RHIPipelineStageFlagBits::eTopOfPipe};
    RDGWriterVisibility writer;
};

struct RDGBufferContentRange
{
    uint64_t begin{0};
    uint64_t end{0}; // Exclusive.
    RDGContentStatus status{RDGContentStatus::eUnknown};
};

struct RDGResourceContent
{
    RDGContentStatus status{RDGContentStatus::eUnknown};

    // Content coverage only. Barrier/dependency range tracking is a later phase.
    HashMap<uint64_t, RDGContentStatus> textureSubresources;
    HeapVector<RDGBufferContentRange> bufferRanges;
    bool hasProducedElements{false}; // Does not imply initialized unused capacity.

    // Retained with physical contents across graph rebuilds; invalidation resets it.
    bool unknownWarningLogged{false};
};

class ResourceStateTracker
{
public:
    explicit ResourceStateTracker(RDGMetrics* metrics = nullptr) : m_metrics(metrics) {}

    ResourceStateTracker(const ResourceStateTracker&) = default;

    ResourceStateTracker(ResourceStateTracker&&) = default;

    ResourceStateTracker& operator=(ResourceStateTracker other)
    {
        m_contents      = std::move(other.m_contents);
        m_textureStates = std::move(other.m_textureStates);
        m_bufferStates  = std::move(other.m_bufferStates);
        // Keep the receiving executor's metrics and invalidate plans on assignment/rollback.
        // Independent trackers can have the same numeric revision but different contents.
        ++m_revision;

        return *this;
    }

    RDGTextureResourceState GetTextureState(const RHITexture* pTexture) const;

    RDGBufferResourceState GetBufferState(const RHIBuffer* pBuffer) const;

    void UpdateTextureState(const RHITexture* pTexture,
                            RHIAccessMode accessMode,
                            RHITextureUsage usage,
                            BitField<RHIPipelineStageFlagBits> pipelineStages);

    void UpdateBufferState(const RHIBuffer* pBuffer,
                           RHIAccessMode accessMode,
                           BitField<RHIBufferUsageFlagBits> usage,
                           BitField<RHIPipelineStageFlagBits> pipelineStages);

    RDGResourceContent GetContents(const RHIResource* resource) const;

    void SetContents(const RHIResource* resource, const RDGResourceContent& contents);

    // Explicit invalidation can advance the revision even when no state was tracked.
    void RemoveResourceState(uint64_t resourceId, bool invalidatePlans = false);

    uint64_t GetRevision() const
    {
        return m_revision;
    }

private:
    friend class RenderGraph;
    friend struct RDGSubmissionTestAccess;
    uint64_t m_revision{0};

    // Graph accesses have already been observed by metrics. External updates invalidate history.
    void SetTextureState(const RHITexture* texture, const RDGTextureResourceState& state);

    void SetBufferState(const RHIBuffer* buffer, const RDGBufferResourceState& state);

    HashMap<uint64_t, RDGResourceContent> m_contents;
    RDGMetrics* m_metrics{nullptr};

    // Resource StableId as Key
    HashMap<uint64_t, RDGTextureResourceState> m_textureStates;
    HashMap<uint64_t, RDGBufferResourceState> m_bufferStates;
};

class RenderGraph;
struct RDGCompiledNode;

class RDGExecutor
{
public:
    explicit RDGExecutor(RenderDevice* pDevice = nullptr) :
        m_pRenderDevice(pDevice), m_resourceStateTracker(&m_metrics)
    {}

    RDGExecutor(const RDGExecutor&) = delete;

    RDGExecutor& operator=(const RDGExecutor&) = delete;

    // CPU recording harness: updates this executor's recording history, without publishing
    // extractions. Use RenderDevice::ExecuteRenderGraph for submission and publication.
    bool Execute(RenderGraph* pGraph, RHICommandList* pCmdList);

    // CPU recording only. Lists are indexed by schedule group ID and remain caller-owned.
    bool ExecuteGroups(RenderGraph* graph,
                       VectorView<RHICommandList*> lists,
                       RDGSchedule& recordedSchedule,
                       VectorView<const RDGExternalQueueState> externalStates = {});

    bool Prepare(RenderGraph* pGraph);

    RDGMetrics& GetMetrics()
    {
        return m_metrics;
    }

    const RDGMetrics& GetMetrics() const
    {
        return m_metrics;
    }

    ResourceStateTracker& GetResourceStateTracker()
    {
        return m_resourceStateTracker;
    }

    const ResourceStateTracker& GetResourceStateTracker() const
    {
        return m_resourceStateTracker;
    }

private:
    friend class RenderDevice;
    friend struct RDGExecutionPlanTestAccess;

    // One CPU execution only. Kept inside RenderCore, with the graph alive through submission.
    struct ExecutionPlan
    {
        ExecutionPlan() = default;

        ExecutionPlan(const ExecutionPlan&) = delete;

        ExecutionPlan& operator=(const ExecutionPlan&) = delete;

        RenderGraph* graph{nullptr};
        uint64_t executorIdentity{0};
        uint64_t buildGeneration{0};
        uint64_t preparationSerial{0};
        uint64_t stateRevision{0};
        double prepareCPUUs{0};
        RDGPassCompileTimings passTimings;
        uint32_t preparationPasses{0};
        bool precompiled{false};
        bool transfer{false};
        RDGSchedule schedule;
        bool consumed{true};
    };

    bool PrepareExecution(RenderGraph* graph, ExecutionPlan& plan);

    bool RefreshExecution(ExecutionPlan& plan);

    bool CheckExecutionPlan(const ExecutionPlan& plan);

    bool ExecutePrepared(ExecutionPlan& plan,
                         RHICommandList* cmdList,
                         const std::function<RHISubmissionResult()>& submit = {},
                         bool deferPublication                              = false);

    bool BuildExecutionPlan(ExecutionPlan& plan);

    bool ExecutePreparedGroups(ExecutionPlan& plan,
                               VectorView<RHICommandList*> lists,
                               VectorView<const RDGExternalQueueState> externalStates = {},
                               const std::function<RHISubmissionResult()>& submit     = {},
                               bool deferPublication                                  = false);

    bool BuildGroupBarriers(ExecutionPlan& plan,
                            VectorView<const RDGExternalQueueState> externalStates,
                            HeapVector<RDGCompiledNode>& nodes);

    bool ExecuteTransaction(ExecutionPlan& plan,
                            VectorView<RHICommandList*> lists,
                            bool grouped,
                            VectorView<const RDGExternalQueueState> externalStates,
                            const std::function<RHISubmissionResult()>& submit,
                            bool deferPublication);

    RenderDevice* m_pRenderDevice{nullptr};

    static uint64_t NextIdentity()
    {
        static std::atomic<uint64_t> next{1};
        return next.fetch_add(1, std::memory_order_relaxed);
    }

    uint64_t m_identity{NextIdentity()};

    bool CompileGraph(RenderGraph* pGraph);

    bool CompileGraphInternal(RenderGraph* pGraph);

    bool AttachGraphBarriers(RenderGraph* pGraph);

    ResourceStateTracker m_resourceStateTracker;
    RDGMetrics m_metrics;
    // Covers recording, submission, and commit/rollback of private tracker copies.
    bool m_executing{false};

    FlatHashMap<RHIBuffer*, RDGBufferResourceState> m_frameBufferStates;
    FlatHashMap<RHITexture*, RDGTextureResourceState> m_frameTextureStates;
};

enum class RDGExecutionState : uint8_t
{
    eIdle,
    eBuilding,
    eRecorded,
    eCompiled,
    eExecuting,
    eInvalid,
};

/*****************************/
/********* RDGNodes **********/
/*****************************/

struct RDGNodeBase
{
    RDG_ID id{-1};
    NameID tag;
    RDGNodeType type{RDGNodeType::eNone};
    RDGQueuePreference queuePreference{RDGQueuePreference::eDefault};
    BitField<RHIPipelineStageFlagBits> selfStages;
    uint32_t accessOffset{0};
    uint32_t accessCount{0};
};

struct RDGPassNode : RDGNodeBase
{
    RDGCompiledPass* pCompiledPass{nullptr};
    int32_t passDescIdx{-1};
    int32_t cmdLambdaIdx{-1};

    // Recording can interleave passes. End() flattens these into the compiled access array.
    HeapVector<RDGAccess> pendingAccesses;
    HeapVector<RDGVersionAccess> versionAccesses;
    HeapVector<RDGContentAccess> contentAccesses;
    bool requireDefinedContents{false};
    bool neverCull{false};
    bool live{true};
};

struct RDGCompiledNode
{
    RDG_ID nodeId{-1};
    RDGQueuePreference queuePreference{RDGQueuePreference::eDefault};
    RDGAsyncComputeEligibility asyncComputeEligibility{RDGAsyncComputeEligibility::eNotRequested};
    RDGQueue plannedQueue{RDGQueue::eGraphics};
    uint32_t submissionGroup{UINT32_MAX};
    BitField<RHIPipelineStageFlagBits> prologueSrcStages;
    BitField<RHIPipelineStageFlagBits> prologueDstStages;
    uint32_t initialBarrierCount{0};
    HeapVector<RDGAccess> initialResourceAccesses;
    HeapVector<RHIBufferTransition> prologueBufferTransitions;
    HeapVector<RHITextureTransition> prologueTextureTransitions;
};

struct RDGCompileStats
{
    uint32_t nodeCount{0};
    uint32_t passCount{0};
    uint32_t resourceCount{0};
    uint32_t dependencyBarrierCount{0};
    uint32_t dependencyEdgeCount{0};
    uint32_t culledPassCount{0};
    uint32_t liveResourceCount{0};
    uint32_t reusedAllocationCount{0};
};

class RenderGraph
{
public:
    RenderGraph(NameID tag) : m_rdgTag(tag), m_poolAlloc(64 * 1024) // 8KB initial size
    {
        m_resourceManager.SetFrameArena(&m_poolAlloc);
        m_resourceManager.SetOwner(this);
    }

    ~RenderGraph()
    {
        Destroy();
    }

    RDGShaderPassCmdRecorder AddGraphicsPass(RDGGraphicsPassDesc desc);

    RDGShaderPassCmdRecorder AddComputePass(RDGComputePassDesc desc);

    RDGTransferPassCmdRecorder AddTransferPass(NameID passName);

    // Preparation result with logical queue/layout requirements; native submission is separate.
    const RDGSchedule& GetSchedule() const
    {
        return m_schedule;
    }

    // Imports are retained from declaration through Reset/Begin/destruction. Submitted uses
    // retire through the execution device, which must outlive this graph.
    bool Reset();

    bool Begin();

    bool End();

    const HeapVector<RDGResult>& GetWarnings() const
    {
        return m_warnings;
    }

    // Resource-version edges, including reasons sharing the same node pair.
    // GetSchedule() additionally includes layout requirements and group ordering.
    // Available after compilation; cleared on Begin/Reset. IDs refer to this build only.
    const HeapVector<RDGDependency>& GetDependencies() const
    {
        return m_dependencies;
    }

    const RDGResult& GetResult() const
    {
        return m_result;
    }

    RDGExecutionState GetExecutionState() const
    {
        return m_executionState;
    }

    RDGResourceManager* GetResourceManager()
    {
        return &m_resourceManager;
    }

    const RDGCompileStats& GetCompileStats() const
    {
        return m_compileStats;
    }

    // Diagnostic comparison switches; set before recording. Renderer interfaces do not change.
    bool SetOptimizations(bool cullPasses, bool reuseAllocations);

private:
    friend class RDGExecutor;
    friend class RDGMetrics;
    friend class RDGPassCompiler;
    friend class RenderDevice;
    friend class RDGPassCmdEncoder;
    friend class RDGResourceManager;
    friend struct RDGExecutionPlanTestAccess;

    bool Fail(RDGErrorCode code, const std::string& message);

    bool Check(bool condition, RDGErrorCode code, const std::string& message);

    void ReleaseCompiledShaderPasses();

    RDGGraphicsPass* AcquireGraphicsPass();

    RDGComputePass* AcquireComputePass();

    void TrimCompiledPassStorage();

    ShaderProgram* ValidatePassDescription(const RDGPassDescBase& desc, bool graphics);

    struct PassBindingValidator;

    bool ValidateShaderIdentity(const RDGPassDescBase& desc);

    bool ValidateShaderIdentities();

    bool ValidateGraphicsDescription(const RDGGraphicsPassDesc& desc);

    bool CheckRecorder(const RDGPassNode* node, uint64_t generation);

    bool ValidateTextureRange(const RDGResourceManager::Allocation* resource,
                              const RHITextureSubResourceRange& range);

    static void SetPipelineStatesForPassNode(RDGPassNode* pPassNode,
                                             BitField<RHIPipelineStageFlagBits> inStageFlags);

    bool DeclareTextureAccessForPass(const RDGPassNode* pPassNode,
                                     const RDGResourceManager::Allocation* pResource,
                                     RHITextureUsage usage,
                                     const RHITextureSubResourceRange& range,
                                     RHIAccessMode accessMode,
                                     BitField<RHIPipelineStageFlagBits> shaderStages = {},
                                     RDGContentEffect intent = RDGContentEffect::eAutomatic,
                                     bool fullCoverage       = true,
                                     bool discardAfter       = false,
                                     RDGResource value       = {});

    bool DeclareBufferAccessForPass(const RDGPassNode* pPassNode,
                                    const RDGResourceManager::Allocation* pResource,
                                    BitField<RHIBufferUsageFlagBits> usage,
                                    RHIAccessMode accessMode,
                                    BitField<RHIPipelineStageFlagBits> shaderStages = {},
                                    RDGContentEffect intent = RDGContentEffect::eAutomatic,
                                    bool fullCoverage       = true,
                                    bool discardAfter       = false,
                                    RDGResource value       = {});

    bool DeclareContentAccess(const RDGPassNode* node,
                              const RDGResourceManager::Allocation* resource,
                              RDGContentEffect intent,
                              const RHITextureSubResourceRange& range = {},
                              bool fullCoverage                       = true,
                              bool discardAfter                       = false);

    template <typename Output> bool ResolveAttachment(Output& output);

    template <typename Output> bool ValidateAttachment(const RDGGraphicsPassDesc& desc,
                                                       const std::string& prefix,
                                                       std::unordered_set<NameID>& tags,
                                                       const Output& out,
                                                       bool depth,
                                                       uint32_t slot);

    template <typename Pass> void ReleaseCompiledPass(Pass* pass, HeapVector<Pass*>& idle);

    bool DeclareTextureBindings(RDGPassNode* node,
                                const RDGPassDescBase* desc,
                                const ShaderProgram* shader,
                                const HeapVector<RDGTextureBinding>& bindings,
                                RHITextureUsage usage);

    bool ApplyContentStatus(const RDGPassNode* node,
                            const RDGContentAccess& access,
                            bool read,
                            bool write,
                            RDGContentStatus produced,
                            std::unordered_set<int32_t>& warned,
                            RDGContentStatus& status);

    bool ApplyContentAccess(std::unordered_set<int32_t>& warned,
                            const RDGPassNode* node,
                            const RDGContentAccess& access,
                            bool read,
                            bool write);

    bool ValidateContents(const ResourceStateTracker& tracker);

    void CommitContents(ResourceStateTracker& tracker);

    bool ResolveAttachments(RDGGraphicsPassDesc& desc);

    bool DeclarePassBindingAccess(RDGPassNode* pPassNode,
                                  ShaderProgram* pShaderProgram,
                                  RDGPassDescBase* pPassDesc);

    bool Execute(VectorView<RHICommandList*> lists,
                 ResourceStateTracker& resourceStateTracker,
                 HeapVector<RDGCompiledNode>& nodes,
                 bool grouped);

    bool CanExecuteOnTransferQueue(const ResourceStateTracker& resourceStateTracker) const;

    void Destroy();

    void ResetBuildState();

    void DestroyNode(RDGNodeBase* pNode);

    static uint64_t CreateNodePairKey(const RDG_ID& nodeId1, const RDG_ID& nodeId2)
    {
        uint64_t key = static_cast<uint64_t>(static_cast<uint32_t>(nodeId1)) << 32;
        key |= static_cast<uint32_t>(nodeId2);

        return key;
    }

    bool RunNode(RDGNodeBase* pNode);

    bool ExecuteGraphicsPass(RDGGraphicsPass* pPass, uint32_t cmdLambdaIdx);

    bool ExecuteComputePass(RDGCompiledPass* pPass, uint32_t cmdLambdaIdx);

    bool ExecuteTransferPass(RDGTransferPass* pPass, uint32_t cmdLambdaIdx);

    uint32_t AddPassCmdLambda(std::function<void(RDGPassCmdEncoder&)> lambda)
    {
        const uint32_t index = static_cast<uint32_t>(m_passCmdLambdas.size());
        m_passCmdLambdas.push_back(std::move(lambda));

        return index;
    }

    void AddDependency(HeapVector<HeapVector<uint32_t>>& adjacency,
                       HashMap<uint64_t, bool>& edges,
                       RDGDependency dependency);

    bool DeclareVersionAccess(const RDGPassNode* node,
                              const RDGResourceManager::Allocation* resource,
                              RDGResource value,
                              RHIAccessMode mode);

    bool BuildVersionDependencies(HeapVector<HeapVector<uint32_t>>& adjacency,
                                  HashMap<uint64_t, bool>& edges);

    bool ReportDependencyCycle(const HeapVector<HeapVector<uint32_t>>& adjacency);

    bool FinalizeResourceVersions();

    bool SortNodesByVersion();

    void DetermineLiveness();

    bool m_cullPasses{true};
    bool m_reuseAllocations{true};

    // Survives Reset: pooled native objects retain this executor's physical state history.
    uint64_t m_executorIdentity{0};

    void BuildCompiledNodeList();

    bool BuildSchedule(const ResourceStateTracker& tracker, bool transferCompatible);
    bool CanPlanOnTransferQueue(const ResourceStateTracker& tracker) const;
    void BuildScheduleDependencies(const ResourceStateTracker& tracker);
    void BuildSubmissionGroups(bool transferCompatible);
    void BuildScheduleResources(const ResourceStateTracker& tracker);
    bool ValidateSchedule();
    RDGAccess GetScheduleAccess(RDG_ID pass, RDG_ID resource) const;
    RDGAccess GetInitialScheduleAccess(RDG_ID resource, const ResourceStateTracker& tracker) const;

    bool AddResourceAccess(RDGPassNode* pNode,
                           RDGResourceManager::Allocation* pResource,
                           const RDGAccess& access);

    void EmitCompiledNodeBarriers(RDGCompiledNode& compiledNode,
                                  ResourceStateTracker& resourceStateTracker);

    void UpdateResourceStatesForNodeAccesses(const RDGCompiledNode& compiledNode,
                                             ResourceStateTracker& resourceStateTracker);

    bool ValidateCompiledGraph();

    template <class T>
        requires std::derived_from<T, RDGNodeBase>
    T* AllocNode(size_t nodeSize)
    {
        // uint32_t nodeDataOffset = m_nodeData.size();
        // m_nodeDataOffset.push_back(nodeDataOffset);
        // m_nodeData.resize(m_nodeData.size() + nodeSize);
        // T* newNode = reinterpret_cast<T*>(&m_nodeData[nodeDataOffset]);
        T* pNewNode = static_cast<T*>(m_poolAlloc.Alloc(nodeSize));
        new (pNewNode) T();
        // *newNode   = T();

        pNewNode->id = m_nodeCount;
        m_nodeCount++;
        m_nodes.push_back(pNewNode);

        return pNewNode;
    }

    const RDGNodeBase* GetNodeBaseById(const RDG_ID& nodeId) const
    {
        return m_nodes[static_cast<uint32_t>(nodeId)];
    }

    RDGNodeBase* GetNodeBaseById(const RDG_ID& nodeId)
    {
        return m_nodes[static_cast<uint32_t>(nodeId)];
    }

    template <class T>
        requires std::derived_from<T, RDGNodeBase>
    static RDGNodeBase* ToBaseNode(T* pDerived)
    {
        return static_cast<RDGNodeBase*>(pDerived);
    }

    template <class T>
        requires std::derived_from<T, RDGNodeBase>
    static const RDGNodeBase* ToBaseNode(const T* pDerived)
    {
        return static_cast<RDGNodeBase*>(pDerived);
    }

    NameID m_rdgTag;

    RenderDevice* m_pRenderDevice{nullptr};

    RHICommandList* m_pCmdList{nullptr};
    RDGMetrics* m_activeMetrics{nullptr}; // Active for captures or continuous validation.

    PoolAllocator<LinearAllocator> m_poolAlloc;

    RDGResourceManager m_resourceManager;

    // nodes
    uint32_t m_nodeCount{0};
    HeapVector<RDG_ID> m_sortedNodes;
    HeapVector<RDGCompiledNode> m_compiledNodes;
    HeapVector<RDGNodeBase*> m_nodes;

    HeapVector<uint32_t> m_inDegrees;
    HeapVector<RDGDependency> m_dependencies;
    RDGSchedule m_schedule;

    // transient output
    struct RDGTransientOutput
    {
        const RDGResourceManager::Allocation* pResource{nullptr};
        RDGPassNode* pProducerPassNode{nullptr};
    };
    HashMap<NameID, RDGTransientOutput> m_transientRTMap;

    HeapVector<RDGAccess> m_accesses;

    // RDG pass data
    HeapVector<RDGGraphicsPassDesc> m_pendingGfxPassDescs;
    HeapVector<RDGComputePassDesc> m_pendingComputePassDescs;
    HeapVector<RDGTransferPassDesc> m_pendingTransferPassDescs;

    HeapVector<RDGGraphicsPass*> m_compiledGfxPasses;
    HeapVector<RDGComputePass*> m_compiledComputePasses;
    HeapVector<RDGTransferPass*> m_compiledXferPasses;

    // CPU scratch storage only; every reuse resolves current bindings and pipeline identities.
    // Bound idle object/vector payload per graph, independently of the native resource pool.
    static constexpr size_t cMaxIdlePassCount = 256;
    static constexpr size_t cMaxIdlePassBytes = 1024 * 1024;
    HeapVector<RDGGraphicsPass*> m_idleGfxPasses;
    HeapVector<RDGComputePass*> m_idleComputePasses;
    size_t m_idlePassBytes{0};

    // RDG pass command lambdas
    HeapVector<std::function<void(RDGPassCmdEncoder&)>> m_passCmdLambdas;

    // RDG states
    RDGExecutionState m_executionState{RDGExecutionState::eIdle};
    RDGResult m_result;
    HeapVector<RDGResult> m_warnings;
    HeapVector<RDGResult> m_pendingContentWarnings;
    HeapVector<RDGResourceContent> m_finalContents;
    RDG_ID m_currentNode;
    bool m_inExecution{false};
    uint64_t m_buildGeneration{0};
    uint64_t m_preparationSerial{0}; // Also invalidates plans prepared by another executor.
    uint32_t m_openTransferRecorders{0};
    RDGCompileStats m_compileStats;
    RDGPassCompileTimings m_passCompileTimings;
    uint32_t m_recordedInitBarrierCount{0};

    friend class RDGShaderPassCmdRecorder;
    friend class RDGTransferPassCmdRecorder;
};
} // namespace zen::rc
