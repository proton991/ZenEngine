#include "Graphics/RenderCore/V2/RenderGraph/RDGMetrics.h"
#include <map>
#include "Graphics/RenderCore/V2/RenderGraph/RenderGraph.h"
#include "Utils/Errors.h"
#include <algorithm>

namespace zen::rc
{
namespace
{
using Clock = MetricsLogger<RDGMetricsSnapshot>::Clock;

double Microseconds(Clock::time_point start)
{
    return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

int64_t ExpandStages(int64_t stages)
{
    if (stages & int64_t(RHIPipelineStageFlagBits::eAllCommands))
    {
        // Host accesses are outside command execution and need an explicit host stage.
        stages |= (1 << 14) - 1;
    }

    if (stages & int64_t(RHIPipelineStageFlagBits::eAllGraphics))
    {
        stages |= ((1 << 11) - 1) & ~1;
    }

    return stages & ((1 << 15) - 1);
}

bool Covers(int64_t mask, int64_t required)
{
    return (mask & required) == required;
}

int64_t WriteAccess(int64_t access)
{
    constexpr int64_t writes = int64_t(RHIAccessFlagBits::eShaderWrite) |
        int64_t(RHIAccessFlagBits::eColorAttachmentWrite) |
        int64_t(RHIAccessFlagBits::eDepthStencilAttachmentWrite) |
        int64_t(RHIAccessFlagBits::eTransferWrite) | int64_t(RHIAccessFlagBits::eHostWrite) |
        int64_t(RHIAccessFlagBits::eMemoryWrite);

    return access & writes;
}

int64_t ExpandAccess(int64_t access)
{
    if (access & int64_t(RHIAccessFlagBits::eMemoryWrite))
    {
        access |= WriteAccess(~int64_t(0));
    }

    if (access & int64_t(RHIAccessFlagBits::eMemoryRead))
    {
        access |= ((1 << 17) - 1) & ~WriteAccess(~int64_t(0));
    }

    return access;
}

static void IncludeAccessStages(int64_t access,
                                int64_t accesses,
                                int64_t supportedStages,
                                int64_t& stages)
{
    if (access & accesses)
    {
        stages |= supportedStages;
    }
}

int64_t AccessStages(int64_t access, int64_t candidates)
{
    using Stage               = RHIPipelineStageFlagBits;
    using Access              = RHIAccessFlagBits;
    constexpr int64_t shaders = int64_t(Stage::eVertexShader) |
        int64_t(Stage::eTessellationControlShader) | int64_t(Stage::eTessellationEvaluationShader) |
        int64_t(Stage::eGeometryShader) | int64_t(Stage::eFragmentShader) |
        int64_t(Stage::eComputeShader);
    int64_t stages = 0;

    IncludeAccessStages(access, int64_t(Access::eIndirectCommandRead),
                        int64_t(Stage::eDrawIndirect), stages);
    IncludeAccessStages(access, int64_t(Access::eIndexRead) | int64_t(Access::eVertexAttributeRead),
                        int64_t(Stage::eVertexInput), stages);
    IncludeAccessStages(access,
                        int64_t(Access::eUniformRead) | int64_t(Access::eShaderRead) |
                            int64_t(Access::eShaderWrite),
                        shaders, stages);
    IncludeAccessStages(access, int64_t(Access::eInputAttachmentRead),
                        int64_t(Stage::eFragmentShader), stages);
    IncludeAccessStages(
        access, int64_t(Access::eColorAttachmentRead) | int64_t(Access::eColorAttachmentWrite),
        int64_t(Stage::eColorAttachmentOutput), stages);
    IncludeAccessStages(access,
                        int64_t(Access::eDepthStencilAttachmentRead) |
                            int64_t(Access::eDepthStencilAttachmentWrite),
                        int64_t(Stage::eEarlyFragmentTests) | int64_t(Stage::eLateFragmentTests),
                        stages);
    IncludeAccessStages(access, int64_t(Access::eTransferRead) | int64_t(Access::eTransferWrite),
                        int64_t(Stage::eTransfer), stages);
    IncludeAccessStages(access, int64_t(Access::eHostRead) | int64_t(Access::eHostWrite),
                        int64_t(Stage::eHost), stages);
    IncludeAccessStages(access, int64_t(Access::eMemoryRead) | int64_t(Access::eMemoryWrite),
                        ~int64_t(0), stages);

    return ExpandStages(candidates) & (access == 0 ? ~int64_t(0) : stages);
}

template <size_t N> static void ExpandExecutionStages(const RHIPipelineStageFlagBits (&pipeline)[N],
                                                      bool source,
                                                      int64_t stages,
                                                      int64_t& result)
{
    int64_t accumulated = 0;

    for (size_t i = 0; i < std::size(pipeline); ++i)
    {
        const int64_t stage = int64_t(pipeline[source ? i : std::size(pipeline) - 1 - i]);
        accumulated |= stage;

        if (stages & stage)
        {
            result |= accumulated;
        }
    }
}

int64_t ExecutionStages(int64_t stages, bool source)
{
    // Logical stage order widens execution scopes only. Memory scopes always use ExpandStages.
    using Stage            = RHIPipelineStageFlagBits;
    stages                 = ExpandStages(stages);
    int64_t result         = stages;
    const Stage graphics[] = {Stage::eTopOfPipe,
                              Stage::eDrawIndirect,
                              Stage::eVertexInput,
                              Stage::eVertexShader,
                              Stage::eTessellationControlShader,
                              Stage::eTessellationEvaluationShader,
                              Stage::eGeometryShader,
                              Stage::eEarlyFragmentTests,
                              Stage::eFragmentShader,
                              Stage::eLateFragmentTests,
                              Stage::eColorAttachmentOutput,
                              Stage::eBottomOfPipe};
    const Stage compute[]  = {Stage::eTopOfPipe, Stage::eDrawIndirect, Stage::eComputeShader,
                              Stage::eBottomOfPipe};
    const Stage transfer[] = {Stage::eTopOfPipe, Stage::eTransfer, Stage::eBottomOfPipe};

    ExpandExecutionStages(graphics, source, stages, result);
    ExpandExecutionStages(compute, source, stages, result);
    ExpandExecutionStages(transfer, source, stages, result);

    return result;
}

bool Contains(const RHITextureSubResourceRange& outer, const RHITextureSubResourceRange& inner)
{
    return Covers(outer.aspect, inner.aspect) && outer.baseMipLevel <= inner.baseMipLevel &&
        uint64_t(outer.baseMipLevel) + outer.levelCount >=
        uint64_t(inner.baseMipLevel) + inner.levelCount &&
        outer.baseArrayLayer <= inner.baseArrayLayer &&
        uint64_t(outer.baseArrayLayer) + outer.layerCount >=
        uint64_t(inner.baseArrayLayer) + inner.layerCount;
}

int64_t BufferAccess(BitField<RHIBufferUsageFlagBits> usage, RHIAccessMode mode)
{
    int64_t flags = 0;

    for (uint32_t i = 0; i < 9; ++i)
    {
        if (int64_t(usage) & (1u << i))
        {
            flags |= RHIBufferUsageToAccessFlagBits(static_cast<RHIBufferUsage>(i + 1), mode);
        }
    }

    return flags;
}

template <typename Resource> uint64_t StableId(const Resource& resource)
{
    return resource.type == RDGResourceType::eTexture ? resource.pTexture->GetStableId() :
                                                        resource.pBuffer->GetStableId();
}

const char* IssueName(RDGMetricIssue issue)
{
    static constexpr const char* names[] = {"missing_barrier",      "stage_coverage",
                                            "access_coverage",      "layout_mismatch",
                                            "range_coverage",       "redundant_barrier_candidate",
                                            "broad_texture_range",  "read_before_write",
                                            "unknown_import_state", "ordering",
                                            "invalid_schedule"};

    return names[static_cast<size_t>(issue)];
}

bool IsOptimizationCandidate(RDGMetricIssue issue)
{
    return issue == RDGMetricIssue::eBroadTextureRange ||
        issue == RDGMetricIssue::eRedundantBarrier;
}

const char* NodeTypeName(RDGNodeType type)
{
    const char* result{};

    switch (type)
    {
        case RDGNodeType::eGraphicsPass: result = "graphics"; break;
        case RDGNodeType::eComputePass: result = "compute"; break;
        case RDGNodeType::eTransferPass: result = "transfer"; break;
        default: result = "none"; break;
    }

    return result;
}

// Keep a single-line, bounded label even when names come from assets.
std::string Label(NameID name)
{
    std::string result(name.CStr(), std::min<size_t>(name.Length(), kMaxRDGNameLen));

    for (char& c : result)
    {
        if (static_cast<unsigned char>(c) < 32 || c == '\"')
        {
            c = '_';
        }
    }

    return result;
}
} // namespace

void RDGBarrierValidator::Seed(const RDGMetricAccess& initial)
{
    State state{};
    state.access               = initial;
    state.hasWriter            = initial.mode == RHIAccessMode::eReadWrite;
    state.writerAccess         = WriteAccess(initial.access);
    state.writerStages         = AccessStages(state.writerAccess, initial.stages);
    m_states[initial.resource] = state;
}

void RDGBarrierValidator::Check(int32_t node,
                                const RDGMetricAccess& next,
                                std::span<const RDGMetricBarrier> barriers,
                                const Reporter& report)
{
    RDGBarrierValidator::State& state = m_states[next.resource];
    const RDGMetricAccess& prior      = state.access;
    const int64_t stages              = ExpandStages(next.stages);
    const bool reads                  = next.mode == RHIAccessMode::eRead;
    const bool layoutChange           = next.texture && prior.layout != next.layout;
    const bool hazard =
        prior.mode != RHIAccessMode::eNone && (prior.mode == RHIAccessMode::eReadWrite || !reads);
    const bool required        = hazard || layoutChange;
    const int32_t previousNode = state.execution == m_execution ? state.node : -1;
    const int32_t writerNode   = state.writerExecution == m_execution ? state.writer : -1;

    if (prior.mode == RHIAccessMode::eNone && reads)
    {
        if (!next.imported)
        {
            report(RDGMetricIssue::eReadBeforeWrite, -1, next.resource);
        }
        else if (next.texture || !next.hostWritten)
        {
            // Imports need tracked GPU state or an explicit host-initialization contract.
            // An unknown import remains an observation, not a proven GPU error.
            report(RDGMetricIssue::eUnknownImportState, -1, next.resource);
        }
    }

    bool found = false, layoutOK = false, rangeOK = false;
    std::array<int64_t, 17> executionCoverage{};
    std::array<std::array<int64_t, 17>, 17> sourceCoverage{};
    std::array<int64_t, 17> destinationCoverage{};
    std::array<std::array<int64_t, 17>, 17> writerCoverage{};
    const bool memoryHazard = prior.mode == RHIAccessMode::eReadWrite || layoutChange;

    for (const RDGMetricBarrier& barrier : barriers)
    {
        if (barrier.resource != next.resource)
        {
            continue;
        }

        found             = true;
        const bool layout = !next.texture ||
            (barrier.oldLayout == prior.layout && barrier.newLayout == next.layout);
        const bool range = next.texture ? Contains(barrier.range, next.range) : barrier.wholeBuffer;
        const int64_t sourceExecution      = ExecutionStages(barrier.srcStages, true);
        const int64_t destinationExecution = ExecutionStages(barrier.dstStages, false);

        for (uint32_t dst = 0; dst < executionCoverage.size(); ++dst)
        {
            if (destinationExecution & (int64_t(1) << dst))
            {
                executionCoverage[dst] |= sourceExecution;
            }
        }

        layoutOK |= layout;
        rangeOK |= range;

        if (layout && range &&
            (WriteAccess(prior.access) == 0 ||
             Covers(ExpandStages(barrier.srcStages),
                    AccessStages(WriteAccess(prior.access), prior.stages))))
        {
            for (uint32_t bit = 0; bit < 17; ++bit)
            {
                if (ExpandAccess(barrier.dstAccess) & (int64_t(1) << bit))
                {
                    const int64_t destinations = AccessStages(int64_t(1) << bit, barrier.dstStages);

                    for (uint32_t dst = 0; dst < 17; ++dst)
                    {
                        if (destinations & (int64_t(1) << dst))
                        {
                            sourceCoverage[dst][bit] |= ExpandAccess(barrier.srcAccess);
                            destinationCoverage[dst] |= int64_t(1) << bit;
                        }
                    }
                }
            }
        }

        if (next.texture && range && !Contains(next.range, barrier.range))
        {
            report(RDGMetricIssue::eBroadTextureRange, previousNode, next.resource);
        }

        // Retain visibility from the last writer across consecutive compatible readers.
        if (state.hasWriter && layout && range &&
            Covers(ExpandStages(barrier.srcStages), state.writerStages))
        {
            const int64_t destinations = ExpandStages(barrier.dstStages);

            for (uint32_t bit = 0; bit < state.visibleAccess.size(); ++bit)
            {
                if (destinations & (int64_t(1) << bit))
                {
                    for (uint32_t accessBit = 0; accessBit < sourceCoverage.size(); ++accessBit)
                    {
                        if ((ExpandAccess(barrier.dstAccess) & (int64_t(1) << accessBit)) &&
                            (AccessStages(int64_t(1) << accessBit, destinations) &
                             (int64_t(1) << bit)))
                        {
                            writerCoverage[bit][accessBit] |= ExpandAccess(barrier.srcAccess);
                        }
                    }
                }
            }
        }
    }

    bool accessOK = true, stageOK = true;

    for (uint32_t dst = 0; dst < executionCoverage.size(); ++dst)
    {
        if ((stages & (int64_t(1) << dst)) &&
            !Covers(executionCoverage[dst], ExpandStages(prior.stages)))
        {
            stageOK = false;
        }
    }

    for (uint32_t bit = 0; bit < sourceCoverage.size(); ++bit)
    {
        for (uint32_t stage = 0; stage < state.visibleAccess.size(); ++stage)
        {
            const int64_t accessBit = int64_t(1) << bit;

            if ((next.access & accessBit) &&
                (AccessStages(accessBit, stages) & (int64_t(1) << stage)) &&
                (!(destinationCoverage[stage] & accessBit) ||
                 !Covers(sourceCoverage[stage][bit], WriteAccess(prior.access))))
            {
                accessOK = false;
            }

            if (state.hasWriter && state.writerAccess != 0 &&
                Covers(writerCoverage[stage][bit], state.writerAccess))
            {
                state.visibleAccess[stage] |= int64_t(1) << bit;
            }
        }
    }

    if (required)
    {
        if (!found)
        {
            report(RDGMetricIssue::eMissingBarrier, previousNode, next.resource);
        }
        else
        {
            if (!stageOK)
            {
                report(RDGMetricIssue::eStageCoverage, previousNode, next.resource);
            }

            if (!layoutOK)
            {
                report(RDGMetricIssue::eLayoutMismatch, previousNode, next.resource);
            }

            if (!rangeOK)
            {
                report(RDGMetricIssue::eRangeCoverage, previousNode, next.resource);
            }

            if (memoryHazard && stageOK && layoutOK && rangeOK && !accessOK)
            {
                report(RDGMetricIssue::eAccessCoverage, previousNode, next.resource);
            }
        }
    }

    bool visible = true;

    if (state.hasWriter && reads)
    {
        for (uint32_t bit = 0; bit < state.visibleAccess.size(); ++bit)
        {
            int64_t requiredAccess = 0;

            for (uint32_t accessBit = 0; accessBit < 17; ++accessBit)
            {
                if ((next.access & (int64_t(1) << accessBit)) &&
                    (AccessStages(int64_t(1) << accessBit, stages) & (int64_t(1) << bit)))
                {
                    requiredAccess |= int64_t(1) << accessBit;
                }
            }

            if (!Covers(state.visibleAccess[bit], requiredAccess))
            {
                visible = false;
            }
        }

        if (!visible && !required)
        {
            report(found ? RDGMetricIssue::eAccessCoverage : RDGMetricIssue::eMissingBarrier,
                   writerNode, next.resource);
        }
    }

    if (found && !required && (!state.hasWriter || visible))
    {
        // A compatible read may still need to acquire visibility from an earlier writer.
        // Do not classify those barriers as redundant just because the last access was a read.
        if (!state.hasWriter || prior.mode == RHIAccessMode::eNone)
        {
            report(RDGMetricIssue::eRedundantBarrier, previousNode, next.resource);
        }
    }

    int64_t combinedStages = next.stages;
    int64_t combinedAccess = next.access;

    if (reads && prior.mode == RHIAccessMode::eRead && !layoutChange)
    {
        combinedStages |= prior.stages;
        combinedAccess |= prior.access;
    }

    state.access        = next;
    state.access.stages = combinedStages;
    state.access.access = combinedAccess;
    state.node          = node;
    state.execution     = m_execution;

    if (!reads)
    {
        state.hasWriter       = true;
        state.writer          = node;
        state.writerAccess    = WriteAccess(next.access);
        state.writerStages    = AccessStages(state.writerAccess, stages);
        state.writerExecution = m_execution;
        state.visibleAccess.fill(0);
    }
}

RDGMetrics::RDGMetrics()
{
    SetSink([](const RDGMetricsSnapshot& sample) {
        if (spdlog::default_logger_raw()->should_log(spdlog::level::info))
        {
            LOGI("{}", RDGMetrics::Format(sample));
        }
    });
}

void RDGMetrics::Configure(const RDGMetricsOptions& options)
{
    if (!options.logging.enabled || !options.validate)
    {
        // There will be an observation gap. Never reuse visibility learned before it.
        m_validator.Reset();
    }

    m_options = options;
    m_logger.Configure(options.logging);
    m_transferLogger.Configure(options.logging);
    m_options.logging = m_logger.GetOptions();
    m_windowExecutions.fill(0);
    m_windowNodes.fill(0);
}

void RDGMetrics::SetSink(Sink sink)
{
    m_logger.SetSink(sink);
    m_transferLogger.SetSink(std::move(sink));
}

void RDGMetrics::RequestCapture(bool transferOnly)
{
    (transferOnly ? m_transferLogger : m_logger).RequestSample();
}

bool RDGMetrics::Begin(RenderGraph& graph, bool precompiled)
{
    bool result{};

    m_capture = false;

    if (m_options.logging.enabled)
    {
        const bool transfer =
            graph.m_pendingGfxPassDescs.empty() && graph.m_pendingComputePassDescs.empty();
        const size_t stream = transfer ? 1 : 0;
        ++m_executions[stream];
        ++m_windowExecutions[stream];
        m_windowNodes[stream] += graph.m_nodeCount;
        m_capture = (transfer ? m_transferLogger : m_logger).TryBeginSample();

        if (!(!m_capture && !m_options.validate))
        {
            m_graph = &graph;

            if (m_options.validate)
            {
                m_validator.BeginGraph();
            }

            if (!m_capture)
            {
                // Replay accesses even between reports so the next capture has complete history.
                result = true;
            }
            else
            {
                // Preserve detail storage capacity between samples.
                HeapVector<RDGNodeMetrics> nodes            = std::move(m_snapshot.nodes);
                HeapVector<RDGMetricDiagnostic> diagnostics = std::move(m_snapshot.diagnostics);
                m_snapshot                                  = {};
                m_snapshot.nodes                            = std::move(nodes);
                m_snapshot.diagnostics                      = std::move(diagnostics);
                m_snapshot.nodes.clear();
                m_snapshot.diagnostics.clear();
                m_snapshot.graph            = graph.m_rdgTag;
                m_snapshot.transferOnly     = transfer;
                m_snapshot.execution        = m_executions[stream];
                m_snapshot.windowExecutions = m_windowExecutions[stream];
                m_snapshot.windowNodes      = m_windowNodes[stream];
                m_windowExecutions[stream] = m_windowNodes[stream] = 0;
                m_snapshot.precompiled                             = precompiled;
                m_snapshot.validated                               = m_options.validate;
                m_snapshot.nodeTimingsEnabled                      = m_options.nodeTimings;
                m_node                                             = {};
                result                                             = true;
            }
        }
    }

    return result;
}

void RDGMetrics::Compiled(RenderGraph& graph,
                          double prepareCPUUs,
                          uint32_t preparationPasses,
                          const RDGPassCompileTimings& passTimings)
{
    if (!m_capture)
    {
        return;
    }

    m_snapshot.passCompileTimings      = passTimings;
    m_snapshot.compileCPUUs            = prepareCPUUs;
    m_snapshot.preparationPasses       = preparationPasses;
    m_snapshot.nodeCount               = uint32_t(graph.m_compiledNodes.size());
    m_snapshot.resources               = graph.m_compileStats.liveResourceCount;
    m_snapshot.dependencyEdges         = graph.m_compileStats.dependencyEdgeCount;
    m_snapshot.dependencyHazards       = graph.m_compileStats.dependencyBarrierCount;
    m_snapshot.culledPasses            = graph.m_compileStats.culledPassCount;
    m_snapshot.reusedAllocations       = graph.m_compileStats.reusedAllocationCount;
    const RDGPoolStats pool            = graph.m_resourceManager.GetPoolStats();
    m_snapshot.assignedTransientBytes  = pool.assignedBytes;
    m_snapshot.availableTransientBytes = pool.availableBytes;
    m_snapshot.retiringTransientBytes  = pool.retiringBytes;

    for (uint32_t i = 0; i < graph.m_resourceManager.m_resources.size(); ++i)
    {
        const RDGResourceManager::Allocation* resource =
            graph.m_resourceManager.FindResourceByIdx(i);

        if (resource->liveAccessCount == 0)
        {
            continue;
        }

        m_snapshot.importedResources += resource->imported;
        m_snapshot.transientResources += !resource->imported && !resource->exported;
    }

    if (m_options.validate)
    {
        ValidateOrder(graph);
    }

    m_executeStart = Clock::now();
}

void RDGMetrics::Report(RDGMetricIssue issue, int32_t node, int32_t previous, uint64_t resource)
{
    if (m_capture)
    {
        ++m_snapshot.issues[static_cast<size_t>(issue)];
        const bool optimization = IsOptimizationCandidate(issue);
        // Whole-image barriers retain their counts without consuming the default detail budget.
        bool record = !optimization || m_options.includeOptimizationDetails;

        if (record && m_snapshot.transferOnly && !m_options.includeTransferNodes)
        {
            ++m_snapshot.omittedDiagnostics;
            record = false;
        }

        HeapVector<RDGMetricDiagnostic>::iterator replacement = m_snapshot.diagnostics.end();

        if (record && m_snapshot.diagnostics.size() >= m_options.maxDiagnosticDetails)
        {
            ++m_snapshot.omittedDiagnostics;

            if (!optimization)
            {
                replacement =
                    std::find_if(m_snapshot.diagnostics.begin(), m_snapshot.diagnostics.end(),
                                 [](const RDGMetricDiagnostic& detail) {
                                     return IsOptimizationCandidate(detail.issue);
                                 });
            }

            // Correctness findings take precedence over optimization details.
            record = replacement != m_snapshot.diagnostics.end();
        }

        if (record)
        {
            RDGMetricDiagnostic diagnostic{issue, node, previous, resource};

            if (node >= 0 && uint32_t(node) < m_graph->m_nodeCount)
            {
                diagnostic.nodeName = m_graph->GetNodeBaseById(node)->tag;
            }

            if (const RDGResourceManager::Allocation* rdgResource =
                    m_graph->m_resourceManager.FindResourceByStableId(resource))
            {
                diagnostic.resourceName = rdgResource->name;
            }

            if (replacement != m_snapshot.diagnostics.end())
            {
                *replacement = std::move(diagnostic);
            }
            else
            {
                m_snapshot.diagnostics.push_back(std::move(diagnostic));
            }
        }
    }
}

void RDGMetrics::ValidateOrder(RenderGraph& graph)
{
    HeapVector<int32_t> order;
    HeapVector<RDGMetricOrderAccess> declarations;
    HeapVector<int32_t> dense(graph.m_nodeCount, -1), original;

    for (const RDGNodeBase* base : graph.m_nodes)
    {
        if (static_cast<const RDGPassNode*>(base)->live)
        {
            dense[base->id] = int32_t(original.size());
            original.push_back(base->id);
        }
    }

    for (RDGCompiledNode const& compiled : graph.m_compiledNodes)
    {
        m_snapshot.reorderedNodes += dense[compiled.nodeId] != int32_t(order.size());
        order.push_back(dense[compiled.nodeId]);
    }

    for (const RDGNodeBase* base : graph.m_nodes)
    {
        const RDGPassNode* node = static_cast<const RDGPassNode*>(base);

        if (!node->live)
        {
            continue;
        }

        for (RDGVersionAccess const& access : node->versionAccesses)
        {
            // Different logical allocations may reuse one physical object. Their version
            // chains remain distinct; native hazards are checked separately by ObserveAccess.
            declarations.push_back(
                {dense[node->id], uint64_t(int32_t(access.resourceId)) + 1, access.writes,
                 int32_t(graph.m_resourceManager.m_versions[access.version].number)});
        }
    }

    RDGBarrierValidator::CheckOrder(
        uint32_t(original.size()), order, declarations,
        [this, &graph, &original](RDGMetricIssue issue, int32_t node, int32_t prior, uint64_t id) {
            const RDGResourceManager::Allocation* resource =
                id ? graph.m_resourceManager.FindResourceByIdx(int32_t(id - 1)) : nullptr;
            Report(issue, node < 0 ? node : original[node], prior < 0 ? prior : original[prior],
                   resource ? StableId(*resource) : 0);
        });
}

static void CheckAccessOrder(
    const HeapVector<int32_t>& positions,
    const std::function<void(RDGMetricIssue, int32_t, int32_t, uint64_t)>& report,
    int32_t previous,
    const RDGMetricOrderAccess& access)
{
    if (previous >= 0 && previous != access.node && positions[previous] >= positions[access.node])
    {
        report(RDGMetricIssue::eOrdering, access.node, previous, access.resource);
    }
}

void RDGBarrierValidator::CheckOrder(
    uint32_t nodeCount,
    std::span<const int32_t> executionOrder,
    std::span<const RDGMetricOrderAccess> declarations,
    const std::function<void(RDGMetricIssue, int32_t, int32_t, uint64_t)>& report)
{
    HeapVector<int32_t> positions(nodeCount, -1);

    for (uint32_t order = 0; order < executionOrder.size(); ++order)
    {
        const int32_t id = executionOrder[order];

        if (id < 0 || size_t(id) >= positions.size() || positions[id] != -1)
        {
            report(RDGMetricIssue::eInvalidSchedule, id, -1, 0);
            continue;
        }

        positions[id] = order;
    }

    for (uint32_t id = 0; id < positions.size(); ++id)
    {
        if (positions[id] == -1)
        {
            report(RDGMetricIssue::eInvalidSchedule, id, -1, 0);
        }
    }

    // Reconstruct constraints from declarations, independent of compiler edges and barriers.

    struct VersionUsers
    {
        const RDGMetricOrderAccess* writer{nullptr};
        HeapVector<const RDGMetricOrderAccess*> readers;
    };

    std::unordered_map<uint64_t, std::map<int32_t, VersionUsers>> versions;

    for (const RDGMetricOrderAccess& access : declarations)
    {
        if (access.node < 0 || uint32_t(access.node) >= nodeCount)
        {
            report(RDGMetricIssue::eInvalidSchedule, access.node, -1, access.resource);
            continue;
        }

        if (access.version < 0)
        {
            report(RDGMetricIssue::eInvalidSchedule, access.node, -1, access.resource);
            continue;
        }

        VersionUsers& use = versions[access.resource][access.version];

        if (access.writes)
        {
            if (access.version == 0 || (use.writer && use.writer->node != access.node))
            {
                report(RDGMetricIssue::eInvalidSchedule, access.node, -1, access.resource);
            }

            use.writer = &access;
        }
        else
        {
            use.readers.push_back(&access);
        }
    }

    for (const std::unordered_map<uint64_t, std::map<int32_t, VersionUsers>>::value_type&
             resourceVersions : versions)
    {
        for (const std::map<int32_t, VersionUsers>::value_type& version : resourceVersions.second)
        {
            if (version.first != 0 && version.second.writer == nullptr)
            {
                report(RDGMetricIssue::eReadBeforeWrite,
                       version.second.readers.empty() ? -1 : version.second.readers.front()->node,
                       -1, resourceVersions.first);
            }

            for (const RDGMetricOrderAccess* reader : version.second.readers)
            {
                if (version.second.writer == nullptr)
                {
                    continue;
                }

                if (version.second.writer->node == reader->node)
                {
                    report(RDGMetricIssue::eInvalidSchedule, reader->node, reader->node,
                           resourceVersions.first);
                }
                else
                {
                    CheckAccessOrder(positions, report, version.second.writer->node, *reader);
                }
            }

            if (version.first == 0 || version.second.writer == nullptr)
            {
                continue;
            }

            const std::map<int, VersionUsers>::const_iterator previous =
                resourceVersions.second.find(version.first - 1);

            if (previous == resourceVersions.second.end())
            {
                if (version.first > 1)
                {
                    report(RDGMetricIssue::eReadBeforeWrite, version.second.writer->node, -1,
                           resourceVersions.first);
                }

                continue; // Version 0 need not be explicitly read.
            }

            if (previous->second.writer)
            {
                CheckAccessOrder(positions, report, previous->second.writer->node,
                                 *version.second.writer);
            }

            for (const RDGMetricOrderAccess* reader : previous->second.readers)
            {
                CheckAccessOrder(positions, report, reader->node, *version.second.writer);
            }
        }
    }
}

void RDGMetrics::BeginNode(RenderGraph& graph, const RDGCompiledNode& compiled)
{
    if (!m_capture)
    {
        return;
    }

    const RDGNodeBase* node = graph.GetNodeBaseById(compiled.nodeId);
    const uint32_t order    = m_node.order;
    m_node                  = {};
    m_node.id               = compiled.nodeId;
    m_node.order            = order;
    m_node.type             = node->type;

    // Only copy labels that will be retained in the bounded report.
    if ((node->type != RDGNodeType::eTransferPass || m_options.includeTransferNodes) &&
        m_snapshot.nodes.size() < m_options.maxNodeDetails)
    {
        m_node.name = node->tag;
    }

    ++m_snapshot.passCounts[static_cast<size_t>(node->type)];

    for (uint32_t i = 0; i < node->accessCount; ++i)
    {
        RDGAccess const& access = graph.m_accesses[node->accessOffset + i];
        m_node.reads += access.accessMode == RHIAccessMode::eRead;
        m_node.writes += access.accessMode == RHIAccessMode::eReadWrite;
    }

    if (m_options.nodeTimings)
    {
        m_nodeStart = Clock::now();
    }
}

void RDGMetrics::ObserveBarriers(RenderGraph& graph,
                                 const RDGCompiledNode& compiled,
                                 const ResourceStateTracker& tracker,
                                 int64_t srcStages,
                                 int64_t dstStages,
                                 VectorView<RHIBufferTransition> buffers,
                                 VectorView<RHITextureTransition> textures,
                                 uint32_t initialResources)
{
    if (m_capture)
    {
        m_node.bufferTransitions  = static_cast<uint32_t>(buffers.size());
        m_node.textureTransitions = static_cast<uint32_t>(textures.size());
        m_node.initialResources   = initialResources;
        m_node.barrierCalls       = !buffers.empty() || !textures.empty();
        m_node.srcStages          = srcStages;
        m_node.dstStages          = dstStages;
    }

    if (!m_options.validate)
    {
        return;
    }

    m_barriers.clear();

    for (RHIBufferTransition const& buffer : buffers)
    {
        RDGMetricBarrier barrier{};
        barrier.resource  = buffer.pBuffer->GetStableId();
        barrier.srcStages = srcStages;
        barrier.dstStages = dstStages;
        barrier.srcAccess = RHIBufferUsageToAccessFlagBits(buffer.oldUsage, buffer.oldAccessMode);
        barrier.srcAccess |= int64_t(buffer.additionalSrcAccess);
        barrier.dstAccess   = RHIBufferUsageToAccessFlagBits(buffer.newUsage, buffer.newAccessMode);
        barrier.wholeBuffer = buffer.offset == 0 &&
            (buffer.size == ZEN_BUFFER_WHOLE_SIZE ||
             buffer.size >= buffer.pBuffer->GetRequiredSize());
        m_barriers.push_back(barrier);
    }

    for (RHITextureTransition const& texture : textures)
    {
        RDGMetricBarrier barrier{};
        barrier.resource  = texture.pTexture->GetStableId();
        barrier.srcStages = srcStages;
        barrier.dstStages = dstStages;
        barrier.srcAccess =
            RHITextureUsageToAccessFlagBits(texture.oldUsage, texture.oldAccessMode);
        barrier.srcAccess |= int64_t(texture.additionalSrcAccess);
        barrier.dstAccess =
            RHITextureUsageToAccessFlagBits(texture.newUsage, texture.newAccessMode);
        barrier.oldLayout = RHITextureUsageToLayout(texture.oldUsage);
        barrier.newLayout = RHITextureUsageToLayout(texture.newUsage);
        barrier.range     = texture.subResourceRange;
        m_barriers.push_back(barrier);
    }

    const RDGNodeBase* node = graph.GetNodeBaseById(compiled.nodeId);

    for (uint32_t i = 0; i < node->accessCount; ++i)
    {
        RDGAccess const& access = graph.m_accesses[node->accessOffset + i];
        const RDGResourceManager::Allocation* resource =
            graph.m_resourceManager.FindResourceByIdx(access.resourceId);
        RDGMetricAccess next{};
        next.resource    = StableId(*resource);
        next.mode        = access.accessMode;
        next.stages      = AccessStages(access.accessFlags, access.pipelineStages);
        next.access      = access.accessFlags;
        next.texture     = resource->type == RDGResourceType::eTexture;
        next.imported    = resource->imported;
        next.hostWritten = resource->hostWritten;
        next.range       = access.textureSubResourceRange;

        if (next.texture)
        {
            next.layout = RHITextureUsageToLayout(access.textureUsage);
        }

        if (!m_validator.HasState(next.resource))
        {
            RDGMetricAccess initial = next;

            if (next.texture)
            {
                const RDGTextureResourceState previous =
                    tracker.GetTextureState(resource->pTexture);
                initial.mode   = previous.accessMode;
                initial.stages = previous.pipelineStages;
                initial.access =
                    RHITextureUsageToAccessFlagBits(previous.usage, previous.accessMode);
                initial.layout = RHITextureUsageToLayout(previous.usage);
            }
            else
            {
                const RDGBufferResourceState previous = tracker.GetBufferState(resource->pBuffer);
                initial.mode                          = previous.accessMode;
                initial.stages                        = previous.pipelineStages;
                initial.access = BufferAccess(previous.usage, previous.accessMode);
            }

            initial.stages = AccessStages(initial.access, initial.stages);
            m_validator.Seed(initial);
        }

        m_validator.Check(node->id, next, m_barriers,
                          [this, node](RDGMetricIssue issue, int32_t previous, uint64_t id) {
                              Report(issue, node->id, previous, id);
                          });
    }
}

void RDGMetrics::EndNode()
{
    if (!m_capture)
    {
        return;
    }

    if (m_options.nodeTimings)
    {
        m_node.recordCPUUs = Microseconds(m_nodeStart);
    }

    RDGNodeMetrics& total = m_snapshot.totals;
    total.reads += m_node.reads;
    total.writes += m_node.writes;
    total.initialResources += m_node.initialResources;
    total.bufferTransitions += m_node.bufferTransitions;
    total.textureTransitions += m_node.textureTransitions;
    total.internalMemoryTransitions += m_node.internalMemoryTransitions;
    total.internalTextureTransitions += m_node.internalTextureTransitions;
    total.barrierCalls += m_node.barrierCalls;
    total.srcStages |= m_node.srcStages;
    total.dstStages |= m_node.dstStages;
    total.recordCPUUs += m_node.recordCPUUs;

    if ((m_node.type != RDGNodeType::eTransferPass || m_options.includeTransferNodes) &&
        m_snapshot.nodes.size() < m_options.maxNodeDetails)
    {
        m_snapshot.nodes.push_back(m_node);
    }
    else
    {
        ++m_snapshot.omittedNodes;
    }

    ++m_node.order;
}

void RDGMetrics::End(double submissionCPUUs)
{
    m_graph = nullptr;

    if (!m_capture)
    {
        return;
    }

    m_snapshot.executeCPUUs = std::max(0.0, Microseconds(m_executeStart) - submissionCPUUs);
    (m_snapshot.transferOnly ? m_transferLogger : m_logger).Publish(m_snapshot);
}

std::string RDGMetrics::Format(const RDGMetricsSnapshot& sample)
{
    const RDGNodeMetrics& t = sample.totals;
    std::string text        = fmt::format(
        "[RDG metrics] graph=\"{}\" stream={} execution={} window(executions={},nodes={}) "
        "sample(nodes={},graphics={},compute={},transfer={},resources={},imported={},transient={},"
        "edges={},dependency_hazards={},reordered={}) "
        "barriers(calls={},buffer={},texture={},initial_resources={},internal_memory={},internal_texture={}) "
        "accesses(read={},read_write={}) cpu_us(compile={:.1f},execute={:.1f}) precompiled={} validated={} preparation_passes={}",
        Label(sample.graph), sample.transferOnly ? "transfer" : "frame", sample.execution,
        sample.windowExecutions, sample.windowNodes, sample.nodeCount, sample.passCounts[1],
        sample.passCounts[2], sample.passCounts[3], sample.resources, sample.importedResources,
        sample.transientResources, sample.dependencyEdges, sample.dependencyHazards,
        sample.reorderedNodes, t.barrierCalls, t.bufferTransitions, t.textureTransitions,
        t.initialResources, t.internalMemoryTransitions, t.internalTextureTransitions, t.reads,
        t.writes, sample.compileCPUUs, sample.executeCPUUs, sample.precompiled, sample.validated,
        sample.preparationPasses);
    text +=
        fmt::format(" liveness(culled={},reused_allocations={}) "
                    "pool_estimated_bytes(assigned={},available={},retiring={})",
                    sample.culledPasses, sample.reusedAllocations, sample.assignedTransientBytes,
                    sample.availableTransientBytes, sample.retiringTransientBytes);
    const RDGPassCompileTimings& setup    = sample.passCompileTimings;
    const PipelineCacheMetrics& pipelines = setup.pipelines;
    text += fmt::format(" pipeline_cache(hits={},misses={},created={},failed={},evicted={})",
                        pipelines.hits, pipelines.misses, pipelines.creations, pipelines.failures,
                        pipelines.evictions);
    text += setup.enabled ?
        fmt::format(" pipeline_cpu_us(key={:.1f},lookup={:.1f},create={:.1f})", pipelines.keyCPUUs,
                    pipelines.lookupCPUUs, pipelines.creationCPUUs) :
        " pipeline_cpu_us=disabled";
    text += setup.enabled ?
        fmt::format(" pass_setup_cpu_us(total={:.1f},bindings={:.1f},pipeline={:.1f})",
                    setup.totalCPUUs, setup.bindingCPUUs, setup.pipelineCPUUs) :
        " pass_setup_cpu_us=disabled";

    for (size_t i = 0; i < sample.issues.size(); ++i)
    {
        if (sample.issues[i] && !IsOptimizationCandidate(RDGMetricIssue(i)))
        {
            text += fmt::format(" {}={}", IssueName(RDGMetricIssue(i)), sample.issues[i]);
        }
    }

    const uint32_t broad     = sample.issues[size_t(RDGMetricIssue::eBroadTextureRange)];
    const uint32_t redundant = sample.issues[size_t(RDGMetricIssue::eRedundantBarrier)];

    if (broad || redundant)
    {
        text += fmt::format(
            " optimization_candidates(broad_texture_range={},redundant_barrier_candidate={})",
            broad, redundant);
    }

    for (const RDGNodeMetrics& node : sample.nodes)
    {
        text += fmt::format(
            "\n  node={} order={} name=\"{}\" type={} read={} read_write={} "
            "barriers(calls={},buffer={},texture={},initial_resources={},internal_memory={},internal_texture={}) "
            "stages=0x{:x}->0x{:x} record_cpu_us={}",
            node.id, node.order, Label(node.name), NodeTypeName(node.type), node.reads, node.writes,
            node.barrierCalls, node.bufferTransitions, node.textureTransitions,
            node.initialResources, node.internalMemoryTransitions, node.internalTextureTransitions,
            node.srcStages, node.dstStages,
            sample.nodeTimingsEnabled ? fmt::format("{:.1f}", node.recordCPUUs) : "disabled");
    }

    for (const RDGMetricDiagnostic& issue : sample.diagnostics)
    {
        text += fmt::format("\n  {}={} node={} name=\"{}\" previous_node={} "
                            "resource_stable_id={} resource=\"{}\"",
                            IsOptimizationCandidate(issue.issue) ? "optimization" : "diagnostic",
                            IssueName(issue.issue), issue.node, Label(issue.nodeName),
                            issue.previousNode, issue.resource, Label(issue.resourceName));
    }

    if (sample.omittedNodes || sample.omittedDiagnostics)
    {
        text += fmt::format("\n  details_omitted(nodes={},diagnostics={})", sample.omittedNodes,
                            sample.omittedDiagnostics);
    }

    return text;
}
} // namespace zen::rc
