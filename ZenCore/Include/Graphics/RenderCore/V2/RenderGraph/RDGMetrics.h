#pragma once

#include "Graphics/RenderCore/V2/RenderGraph/RDGDefs.h"
#include "Graphics/RenderCore/V2/PipelineCacheMetrics.h"
#include "Templates/VectorView.h"
#include "Utils/MetricsLogger.h"
#include <array>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace zen::rc
{
class RenderGraph;
class ResourceStateTracker;
struct RDGCompiledNode;

enum class RDGMetricIssue : uint8_t
{
    eMissingBarrier,
    eStageCoverage,
    eAccessCoverage,
    eLayoutMismatch,
    eRangeCoverage,
    eRedundantBarrier,
    eBroadTextureRange,
    eReadBeforeWrite,
    eUnknownImportState,
    eOrdering,
    eInvalidSchedule,
    eCount
};

struct RDGMetricDiagnostic
{
    RDGMetricIssue issue{};
    int32_t node{-1};
    int32_t previousNode{-1};
    uint64_t resource{0}; // Physical stable ID, also used in barrier diagnostics.
    NameID nodeName;
    NameID resourceName;
};

// Normalized access/barrier records keep the checker independent of the compiler's
// barrier decisions, making it possible to test absent/corrupt synchronization.
struct RDGMetricAccess
{
    uint64_t resource{0};
    RHIAccessMode mode{RHIAccessMode::eNone};
    int64_t stages{0};
    int64_t access{0};
    bool texture{false};
    RHITextureLayout layout{RHITextureLayout::eUndefined};
    RHITextureSubResourceRange range;
    bool imported{true};
    bool hostWritten{false}; // Initialized buffer reads are visible through queue submission.
};

struct RDGMetricBarrier
{
    uint64_t resource{0};
    int64_t srcStages{0};
    int64_t dstStages{0};
    int64_t srcAccess{0};
    int64_t dstAccess{0};
    RHITextureLayout oldLayout{RHITextureLayout::eUndefined};
    RHITextureLayout newLayout{RHITextureLayout::eUndefined};
    RHITextureSubResourceRange range;
    bool wholeBuffer{true};
};

struct RDGMetricOrderAccess
{
    int32_t node{-1};
    uint64_t resource{0};
    bool writes{false};
    int32_t version{0}; // 0 initial contents; writes define versions 1..N. Negative is invalid.
};

class RDGBarrierValidator
{
public:
    using Reporter = std::function<void(RDGMetricIssue, int32_t, uint64_t)>;

    void Reset()
    {
        m_states.clear();
    }

    void BeginGraph()
    {
        ++m_execution;
    }

    bool HasState(uint64_t resource) const
    {
        return m_states.contains(resource);
    }

    void Forget(uint64_t resource)
    {
        m_states.erase(resource);
    }

    void Seed(const RDGMetricAccess& initial);

    // Reports previous node and resource. The caller supplies the current node.
    void Check(int32_t node,
               const RDGMetricAccess& access,
               std::span<const RDGMetricBarrier> barriers,
               const Reporter& report);

    // Node IDs are dense lookup keys [0, nodeCount). Version declarations may be in any order.
    static void CheckOrder(
        uint32_t nodeCount,
        std::span<const int32_t> executionOrder,
        std::span<const RDGMetricOrderAccess> declarations,
        const std::function<void(RDGMetricIssue, int32_t, int32_t, uint64_t)>& report);

private:
    struct State
    {
        RDGMetricAccess access;
        int32_t node{-1};
        int32_t writer{-1};
        uint64_t execution{0};
        uint64_t writerExecution{0};
        bool hasWriter{false};
        int64_t writerStages{0};
        int64_t writerAccess{0};

        // Visibility is tracked per destination stage, not just a union of masks.
        std::array<int64_t, 17> visibleAccess{};
    };
    std::unordered_map<uint64_t, State> m_states;
    uint64_t m_execution{0};
};

struct RDGNodeMetrics
{
    int32_t id{-1};
    uint32_t order{0};
    NameID name;
    RDGNodeType type{RDGNodeType::eNone};
    uint32_t reads{0};
    uint32_t writes{0};
    uint32_t initialResources{0};
    uint32_t bufferTransitions{0};
    uint32_t textureTransitions{0};
    uint32_t internalMemoryTransitions{0};
    uint32_t internalTextureTransitions{0};
    uint32_t barrierCalls{0};
    int64_t srcStages{0};
    int64_t dstStages{0};
    double recordCPUUs{0}; // CPU command recording; valid only when snapshot.nodeTimingsEnabled.
};

// Binding and pipeline timings are subsets of total pass setup, not additional phases.
struct RDGPassCompileTimings
{
    bool enabled{false};
    double totalCPUUs{0};
    double bindingCPUUs{0};
    double pipelineCPUUs{0};
    PipelineCacheMetrics pipelines;

    void Accumulate(const RDGPassCompileTimings& other)
    {
        enabled |= other.enabled;
        totalCPUUs += other.totalCPUUs;
        bindingCPUUs += other.bindingCPUUs;
        pipelineCPUUs += other.pipelineCPUUs;
        pipelines.Accumulate(other.pipelines);
    }
};

struct RDGMetricsSnapshot
{
    NameID graph;
    uint64_t execution{0};
    uint64_t windowExecutions{0};
    uint64_t windowNodes{0};
    bool transferOnly{false};
    bool precompiled{false}; // Graph was compiled before this execution's preparation began.
    bool validated{false};
    bool nodeTimingsEnabled{false}; // Captured configuration; false means unmeasured, not zero.
    uint32_t resources{0};
    uint32_t importedResources{0};
    uint32_t transientResources{0};
    uint32_t dependencyEdges{0};
    uint32_t culledPasses{0};
    uint32_t reusedAllocations{0};
    uint64_t assignedTransientBytes{0};
    uint64_t availableTransientBytes{0};
    uint64_t retiringTransientBytes{0};
    uint32_t dependencyHazards{0};
    uint32_t reorderedNodes{0};
    uint32_t nodeCount{0};
    std::array<uint32_t, 4> passCounts{};
    RDGNodeMetrics totals;
    double compileCPUUs{0}; // All preparation/refresh work for this execution, excluding uploads.
    uint32_t preparationPasses{0}; // Full compile/barrier passes, including state refreshes.
    RDGPassCompileTimings passCompileTimings;
    double executeCPUUs{0}; // Includes sampled diagnostics; excludes formatting and sink I/O.
    std::array<uint32_t, static_cast<size_t>(RDGMetricIssue::eCount)> issues{};
    uint32_t omittedNodes{0};
    uint32_t omittedDiagnostics{0};
    std::vector<RDGNodeMetrics> nodes;
    std::vector<RDGMetricDiagnostic> diagnostics;
};

struct RDGMetricsOptions
{
    MetricsLogOptions logging;
    bool validate{true};
    bool nodeTimings{false};
    bool preparationTimings{false}; // Opt-in per-pass setup/binding/pipeline CPU attribution.
    bool includeTransferNodes{false};
    uint32_t maxNodeDetails{32};
    uint32_t maxDiagnosticDetails{16};

    // Counts are always collected. Per-node optimization candidates are opt-in.
    bool includeOptimizationDetails{false};
};

class RDGMetrics
{
public:
    // Configure/use on the RDG executor's thread, outside an active capture. Sinks are
    // synchronous; snapshots own their labels and can be copied for later inspection.
    using Sink = MetricsLogger<RDGMetricsSnapshot>::Sink;

    RDGMetrics();

    void Configure(const RDGMetricsOptions& options);

    const RDGMetricsOptions& GetOptions() const
    {
        return m_options;
    }

    void SetSink(Sink sink);

    void RequestCapture(bool transferOnly = false);

    const RDGMetricsSnapshot& GetLastSnapshot() const
    {
        return m_snapshot;
    }

    static std::string Format(const RDGMetricsSnapshot& snapshot);

private:
    friend class RDGExecutor;
    friend class RenderGraph;
    friend class ResourceStateTracker;
    friend struct RDGSubmissionTestAccess;

    void ForgetResource(uint64_t resource)
    {
        m_validator.Forget(resource);
    }

    bool Begin(RenderGraph& graph, bool precompiled);

    void Compiled(RenderGraph& graph,
                  double prepareCPUUs,
                  uint32_t preparationPasses,
                  const RDGPassCompileTimings& passTimings);

    void BeginNode(RenderGraph& graph, const RDGCompiledNode& compiled);

    void ObserveBarriers(RenderGraph& graph,
                         const RDGCompiledNode& compiled,
                         const ResourceStateTracker& tracker,
                         int64_t srcStages,
                         int64_t dstStages,
                         VectorView<RHIBufferTransition> buffers,
                         VectorView<RHITextureTransition> textures,
                         uint32_t initialResources);

    void EndNode();

    void End(double submissionCPUUs = 0);

    void Report(RDGMetricIssue issue, int32_t node, int32_t previous, uint64_t resource);

    void ValidateOrder(RenderGraph& graph);

    RDGMetricsOptions m_options;
    MetricsLogger<RDGMetricsSnapshot> m_logger;
    MetricsLogger<RDGMetricsSnapshot> m_transferLogger;
    RDGMetricsSnapshot m_snapshot;
    RDGNodeMetrics m_node;
    RDGBarrierValidator m_validator;
    std::vector<RDGMetricBarrier> m_barriers;
    bool m_capture{false};
    std::array<uint64_t, 2> m_executions{};
    std::array<uint64_t, 2> m_windowExecutions{};
    std::array<uint64_t, 2> m_windowNodes{};
    MetricsLogger<RDGMetricsSnapshot>::Clock::time_point m_executeStart, m_nodeStart;
    RenderGraph* m_graph{nullptr}; // Borrowed only during capture, never retained in a snapshot.
};
} // namespace zen::rc
