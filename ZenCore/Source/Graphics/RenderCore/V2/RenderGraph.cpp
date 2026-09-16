#include "Graphics/RenderCore/V2/RenderGraph/RenderGraph.h"
#include "Graphics/RHI/RHICommon.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RHI/RHICommandList.h"
#include "Graphics/RenderCore/V2/RenderGraph/RDGDefs.h"
#include "Graphics/RenderCore/V2/RenderGraph/RDGPassCompiler.h"
#include "Utils/Errors.h"

#include <algorithm>
#include <limits>
#include <queue>
#include <unordered_set>

namespace zen::rc
{
namespace
{
constexpr int64_t kShaderStages = int64_t(RHIPipelineStageFlagBits::eVertexShader) |
    int64_t(RHIPipelineStageFlagBits::eTessellationControlShader) |
    int64_t(RHIPipelineStageFlagBits::eTessellationEvaluationShader) |
    int64_t(RHIPipelineStageFlagBits::eGeometryShader) |
    int64_t(RHIPipelineStageFlagBits::eFragmentShader) |
    int64_t(RHIPipelineStageFlagBits::eComputeShader);

bool ValidContentGuarantee(const RHIShaderResourceDescriptor& descriptor,
                           RDGContentGuarantee contents)
{
    const bool buffer = descriptor.type == RHIShaderResourceType::eStorageBuffer;
    bool valid        = false;

    switch (contents)
    {
        case RDGContentGuarantee::eNone: valid = descriptor.readable || descriptor.writable; break;
        case RDGContentGuarantee::eDiscard:
        case RDGContentGuarantee::eFullWrite: valid = descriptor.writable; break;
        case RDGContentGuarantee::eProducedElements: valid = buffer && descriptor.writable; break;
        case RDGContentGuarantee::eConsumeProducedElements:
            valid = buffer && descriptor.readable && !descriptor.writable;
            break;
    }

    return valid;
}

RDGContentEffect BindingEffect(const RHIShaderResourceDescriptor& descriptor,
                               RDGContentGuarantee contents)
{
    RDGContentEffect effect = !descriptor.writable ? RDGContentEffect::eRead :
        descriptor.readable                        ? RDGContentEffect::eReadWrite :
                                                     RDGContentEffect::eWrite;

    switch (contents)
    {
        case RDGContentGuarantee::eDiscard: effect = RDGContentEffect::eDiscardWrite; break;
        case RDGContentGuarantee::eFullWrite: effect = RDGContentEffect::eFullWrite; break;
        case RDGContentGuarantee::eProducedElements:
            effect = RDGContentEffect::eWriteProducedElements;
            break;
        case RDGContentGuarantee::eConsumeProducedElements:
            effect = RDGContentEffect::eReadProducedElements;
            break;
        case RDGContentGuarantee::eNone: break;
    }

    return effect;
}

RHIAccessMode BindingMode(const RHIShaderResourceDescriptor& descriptor)
{
    // Current RHI conservatively groups writes with read/write. Exact backend masks are Phase 4.
    return descriptor.writable ? RHIAccessMode::eReadWrite : RHIAccessMode::eRead;
}

// A partial clear preserves defined pixels outside the render area. It cannot
// initialize an otherwise undefined whole attachment, but it does not discard it.
RDGContentEffect AttachmentIntent(RHIRenderTargetLoadOp loadOp, bool fullWrite, bool fullCoverage)
{
    RDGContentEffect result{};

    if (loadOp == RHIRenderTargetLoadOp::eLoad)
    {
        result = RDGContentEffect::eReadWrite;
    }
    else if (loadOp == RHIRenderTargetLoadOp::eClear)
    {
        result = fullCoverage ? RDGContentEffect::eFullWrite : RDGContentEffect::eWrite;
    }
    else
    {
        result = fullWrite && fullCoverage ? RDGContentEffect::eFullWrite :
                                             RDGContentEffect::eDiscardWrite;
    }

    return result;
}

bool IsProducedElementIntent(RDGContentEffect intent)
{
    return intent == RDGContentEffect::eReadProducedElements ||
        intent == RDGContentEffect::eWriteProducedElements;
}

static void IncludeContentStatus(RDGContentStatus& result, RDGContentStatus status)
{
    if (status == RDGContentStatus::eUndefined ||
        (status == RDGContentStatus::eUnknown && result == RDGContentStatus::eDefined))
    {
        result = status;
    }
}

RDGContentStatus BufferContents(const RDGResourceContent& contents, uint64_t begin, uint64_t size)
{
    const uint64_t end      = begin + size;
    uint64_t cursor         = begin;
    RDGContentStatus result = RDGContentStatus::eDefined;

    for (const RDGBufferContentRange& range : contents.bufferRanges)
    {
        if (range.end <= cursor)
        {
            continue;
        }

        if (range.begin >= end)
        {
            break;
        }

        if (range.begin > cursor)
        {
            IncludeContentStatus(result, contents.status);
        }

        IncludeContentStatus(result, range.status);
        cursor = std::min(end, range.end);
    }

    if (cursor < end)
    {
        IncludeContentStatus(result, contents.status);
    }

    return result;
}

void SetBufferContents(RDGResourceContent& contents,
                       uint64_t begin,
                       uint64_t size,
                       uint64_t bufferSize,
                       RDGContentStatus status)
{
    const uint64_t end = begin + size;

    if (begin == 0 && size == bufferSize)
    {
        contents.status = status;
        contents.bufferRanges.clear();

        return;
    }

    HeapVector<RDGBufferContentRange> updated;
    updated.reserve(contents.bufferRanges.size() + 2);

    for (RDGBufferContentRange const& range : contents.bufferRanges)
    {
        if (range.end <= begin || range.begin >= end)
        {
            updated.push_back(range);
        }
        else
        {
            if (range.begin < begin)
            {
                updated.push_back({range.begin, begin, range.status});
            }

            if (range.end > end)
            {
                updated.push_back({end, range.end, range.status});
            }
        }
    }

    if (status != contents.status)
    {
        updated.push_back({begin, end, status});
    }

    std::sort(updated.begin(), updated.end(),
              [](const RDGBufferContentRange& lhs, const RDGBufferContentRange& rhs) {
                  return lhs.begin < rhs.begin;
              });
    contents.bufferRanges.clear();

    for (RDGBufferContentRange const& range : updated)
    {
        if (!contents.bufferRanges.empty() && contents.bufferRanges.back().end == range.begin &&
            contents.bufferRanges.back().status == range.status)
        {
            contents.bufferRanges.back().end = range.end;
        }
        else
        {
            contents.bufferRanges.push_back(range);
        }
    }

    if (contents.bufferRanges.size() == 1 && contents.bufferRanges[0].begin == 0 &&
        contents.bufferRanges[0].end == bufferSize)
    {
        contents.status = contents.bufferRanges[0].status;
        contents.bufferRanges.clear();
    }
}

RHIShaderResourceType LogicalBindingType(RDGBindingType type)
{
    RHIShaderResourceType result{};

    switch (type)
    {
        case RDGBindingType::eStorageBuffer: result = RHIShaderResourceType::eStorageBuffer; break;
        case RDGBindingType::eUniformBuffer: result = RHIShaderResourceType::eUniformBuffer; break;
        case RDGBindingType::eStorageImage: result = RHIShaderResourceType::eImage; break;
        default: result = RHIShaderResourceType::eSamplerWithTexture; break;
    }

    return result;
}

BitField<RHIPipelineStageFlagBits> ShaderPipelineStages(BitField<RHIShaderStageFlagBits> flags)
{
    BitField<RHIPipelineStageFlagBits> stages;
    const std::pair<RHIShaderStageFlagBits, RHIPipelineStageFlagBits> mapping[] = {
        {RHIShaderStageFlagBits::eVertex, RHIPipelineStageFlagBits::eVertexShader},
        {RHIShaderStageFlagBits::eFragment, RHIPipelineStageFlagBits::eFragmentShader},
        {RHIShaderStageFlagBits::eTesselationControl,
         RHIPipelineStageFlagBits::eTessellationControlShader},
        {RHIShaderStageFlagBits::eTesselationEvaluation,
         RHIPipelineStageFlagBits::eTessellationEvaluationShader},
        {RHIShaderStageFlagBits::eGeometry, RHIPipelineStageFlagBits::eGeometryShader},
        {RHIShaderStageFlagBits::eCompute, RHIPipelineStageFlagBits::eComputeShader},
    };

    for (const std::pair<RHIShaderStageFlagBits, RHIPipelineStageFlagBits>& stageMapping : mapping)
    {
        if (flags.HasFlag(stageMapping.first))
        {
            stages.SetFlag(stageMapping.second);
        }
    }

    return stages;
}

BitField<RHIPipelineStageFlagBits> ResourceStages(const RDGPassNode* node,
                                                  BitField<RHIPipelineStageFlagBits> usageStages,
                                                  BitField<RHIPipelineStageFlagBits> shaderStages)
{
    int64_t stages = usageStages;

    if (stages & kShaderStages)
    {
        // Older/custom shader metadata may omit reflection stages; restrict the fallback to
        // shader stages in this pass, never its attachments or indirect arguments.
        stages = (stages & ~kShaderStages) |
            (shaderStages.IsEmpty() ? int64_t(node->selfStages) & kShaderStages :
                                      int64_t(shaderStages));
    }

    return BitField<RHIPipelineStageFlagBits>(stages);
}

// Memory scopes do not inherit the logical execution order of graphics stages.
static void IncludeMemoryStages(int64_t access,
                                int64_t accesses,
                                int64_t stages,
                                int64_t& supported)
{
    if (access & accesses)
    {
        supported |= stages;
    }
}

int64_t MemoryStages(int64_t access, int64_t candidates)
{
    using Stage  = RHIPipelineStageFlagBits;
    using Access = RHIAccessFlagBits;

    if (candidates & int64_t(Stage::eAllCommands))
    {
        candidates |= (1 << 14) - 1;
    }

    if (candidates & int64_t(Stage::eAllGraphics))
    {
        candidates |= ((1 << 11) - 1) & ~1;
    }

    int64_t supported = 0;

    IncludeMemoryStages(access, int64_t(Access::eIndirectCommandRead),
                        int64_t(Stage::eDrawIndirect), supported);
    IncludeMemoryStages(access, int64_t(Access::eIndexRead) | int64_t(Access::eVertexAttributeRead),
                        int64_t(Stage::eVertexInput), supported);
    IncludeMemoryStages(access,
                        int64_t(Access::eUniformRead) | int64_t(Access::eShaderRead) |
                            int64_t(Access::eShaderWrite),
                        kShaderStages, supported);
    IncludeMemoryStages(access, int64_t(Access::eInputAttachmentRead),
                        int64_t(Stage::eFragmentShader), supported);
    IncludeMemoryStages(
        access, int64_t(Access::eColorAttachmentRead) | int64_t(Access::eColorAttachmentWrite),
        int64_t(Stage::eColorAttachmentOutput), supported);
    IncludeMemoryStages(access,
                        int64_t(Access::eDepthStencilAttachmentRead) |
                            int64_t(Access::eDepthStencilAttachmentWrite),
                        int64_t(Stage::eEarlyFragmentTests) | int64_t(Stage::eLateFragmentTests),
                        supported);
    IncludeMemoryStages(access, int64_t(Access::eTransferRead) | int64_t(Access::eTransferWrite),
                        int64_t(Stage::eTransfer), supported);
    IncludeMemoryStages(access, int64_t(Access::eHostRead) | int64_t(Access::eHostWrite),
                        int64_t(Stage::eHost), supported);
    IncludeMemoryStages(access, int64_t(Access::eMemoryRead) | int64_t(Access::eMemoryWrite),
                        ~int64_t(0), supported);

    return candidates & ((1 << 15) - 1) & supported;
}

RDGWriterVisibility NewWriter(int64_t access, int64_t stages)
{
    using Access             = RHIAccessFlagBits;
    constexpr int64_t writes = int64_t(Access::eShaderWrite) |
        int64_t(Access::eColorAttachmentWrite) | int64_t(Access::eDepthStencilAttachmentWrite) |
        int64_t(Access::eTransferWrite) | int64_t(Access::eHostWrite) |
        int64_t(Access::eMemoryWrite);
    RDGWriterVisibility writer{};
    writer.access    = access & writes;
    writer.hasWriter = !writer.access.IsEmpty();
    writer.stages    = MemoryStages(writer.access, stages);

    return writer;
}

template <typename Allocation>
RDGTextureResourceState InitialTextureState(const Allocation* resource,
                                            const ResourceStateTracker& tracker)
{
    RDGTextureResourceState returnValue{};

    if (!resource->hasInitialState)
    {
        returnValue = tracker.GetTextureState(resource->pTexture);
    }
    else
    {
        const RDGTextureImportState& initial = resource->initialTextureState;
        RDGTextureResourceState result{initial.accessMode, initial.usage, initial.stages};
        result.writer = NewWriter(
            RHITextureUsageToAccessFlagBits(initial.usage, initial.accessMode), initial.stages);
        returnValue = result;
    }

    return returnValue;
}

template <typename Allocation>
RDGBufferResourceState InitialBufferState(const Allocation* resource,
                                          const ResourceStateTracker& tracker)
{
    RDGBufferResourceState returnValue{};

    if (!resource->hasInitialState)
    {
        returnValue = tracker.GetBufferState(resource->pBuffer);
    }
    else
    {
        const RDGBufferImportState& initial = resource->initialBufferState;
        RDGBufferResourceState result{initial.accessMode, initial.usage, initial.stages};
        int64_t accesses = 0;

        for (uint32_t bit = 0; bit < 9; ++bit)
        {
            if (int64_t(initial.usage) & (1u << bit))
            {
                accesses |= int64_t(RHIBufferUsageToAccessFlagBits(
                    static_cast<RHIBufferUsage>(bit + 1), initial.accessMode));
            }
        }

        result.writer = NewWriter(accesses, initial.stages);
        returnValue   = result;
    }

    return returnValue;
}

bool NeedsVisibility(const RDGWriterVisibility& writer, const RDGAccess& next)
{
    bool needsVisibility = false;

    if (writer.hasWriter)
    {
        for (size_t bit = 0; bit < writer.visibleStages.size(); ++bit)
        {
            if ((int64_t(next.accessFlags) & (int64_t(1) << bit)) &&
                (MemoryStages(int64_t(1) << bit, next.pipelineStages) & ~writer.visibleStages[bit]))
            {
                needsVisibility = true;
                break;
            }
        }
    }

    return needsVisibility;
}

bool NeedsResourceBarrier(const RDGBufferResourceState& previous, const RDGAccess& next)
{
    bool result{};

    // Buffers have no layout transition. With no prior access there is nothing to synchronize;
    // unknown external reads are still diagnosed independently by metrics.
    if (!(previous.accessMode == RHIAccessMode::eNone))
    {
        result = previous.accessMode != RHIAccessMode::eRead ||
            next.accessMode != RHIAccessMode::eRead || NeedsVisibility(previous.writer, next);
    }

    return result;
}

bool NeedsResourceBarrier(const RDGTextureResourceState& previous, const RDGAccess& next)
{
    return previous.accessMode != RHIAccessMode::eRead || next.accessMode != RHIAccessMode::eRead ||
        RHITextureUsageToLayout(previous.usage) != RHITextureUsageToLayout(next.textureUsage) ||
        NeedsVisibility(previous.writer, next);
}

void AdvanceVisibility(RDGWriterVisibility& writer,
                       const RDGAccess& next,
                       int64_t barrierDestinations,
                       bool layoutChanged = false)
{
    if (next.accessMode == RHIAccessMode::eReadWrite)
    {
        writer = NewWriter(next.accessFlags, next.pipelineStages);
        return;
    }

    if (layoutChanged)
    {
        // Layout transitions can write image memory. Their writes are automatically available,
        // but subsequent readers still need visibility in their own destination memory scopes.
        writer.hasWriter = true;
        writer.visibleStages.fill(0);
    }

    if (writer.hasWriter)
    {
        for (size_t bit = 0; bit < writer.visibleStages.size(); ++bit)
        {
            if (int64_t(next.accessFlags) & (int64_t(1) << bit))
            {
                writer.visibleStages[bit] |= MemoryStages(int64_t(1) << bit, barrierDestinations);
            }
        }
    }
}

RDGBufferResourceState AdvanceState(RDGBufferResourceState previous,
                                    const RDGAccess& next,
                                    int64_t barrierDestinations)
{
    const bool barrier = NeedsResourceBarrier(previous, next);
    const bool readers =
        previous.accessMode == RHIAccessMode::eRead && next.accessMode == RHIAccessMode::eRead;
    AdvanceVisibility(previous.writer, next, barrier ? barrierDestinations : 0);
    previous.accessMode = next.accessMode;
    previous.usage      = int64_t(next.bufferUsage) | (readers ? int64_t(previous.usage) : 0);
    previous.pipelineStages =
        int64_t(next.pipelineStages) | (readers ? int64_t(previous.pipelineStages) : 0);

    return previous;
}

RDGTextureResourceState AdvanceState(RDGTextureResourceState previous,
                                     const RDGAccess& next,
                                     int64_t barrierDestinations)
{
    const bool barrier = NeedsResourceBarrier(previous, next);
    const bool layoutChanged =
        RHITextureUsageToLayout(previous.usage) != RHITextureUsageToLayout(next.textureUsage);
    const bool readers = previous.accessMode == RHIAccessMode::eRead &&
        next.accessMode == RHIAccessMode::eRead && !layoutChanged;
    AdvanceVisibility(previous.writer, next, barrier ? barrierDestinations : 0, layoutChanged);
    previous.accessMode = next.accessMode;
    previous.usage      = next.textureUsage;
    previous.pipelineStages =
        int64_t(next.pipelineStages) | (readers ? int64_t(previous.pipelineStages) : 0);

    return previous;
}

template <typename Allocation> RHIResource* PhysicalResource(const Allocation* resource)
{
    return resource->type == RDGResourceType::eTexture ?
        static_cast<RHIResource*>(resource->pTexture) :
        resource->pBuffer;
}

bool TransferStages(BitField<RHIPipelineStageFlagBits> stages)
{
    constexpr int64_t allowed = int64_t(RHIPipelineStageFlagBits::eTopOfPipe) |
        int64_t(RHIPipelineStageFlagBits::eTransfer) |
        int64_t(RHIPipelineStageFlagBits::eBottomOfPipe);
    return (int64_t(stages) & ~allowed) == 0;
}

// The RDG can combine usages while the RHI transition API takes a single usage.
// Emit all source/destination combinations to preserve every access in that union.
void AddBufferTransitions(HeapVector<RHIBufferTransition>& transitions,
                          RHIBuffer* buffer,
                          RHIAccessMode oldMode,
                          RHIAccessMode newMode,
                          BitField<RHIBufferUsageFlagBits> oldUsage,
                          BitField<RHIBufferUsageFlagBits> newUsage,
                          BitField<RHIAccessFlagBits> earlierWrites)
{
    for (uint32_t source = 0; source < 10; ++source)
    {
        if (source == 0 ? !oldUsage.IsEmpty() : !(int64_t(oldUsage) & (1u << (source - 1))))
        {
            continue;
        }

        for (uint32_t destination = 0; destination < 10; ++destination)
        {
            if (destination == 0 ? !newUsage.IsEmpty() :
                                   !(int64_t(newUsage) & (1u << (destination - 1))))
            {
                continue;
            }

            RHIBufferTransition transition{};
            transition.pBuffer             = buffer;
            transition.oldAccessMode       = oldMode;
            transition.newAccessMode       = newMode;
            transition.oldUsage            = static_cast<RHIBufferUsage>(source);
            transition.newUsage            = static_cast<RHIBufferUsage>(destination);
            transition.additionalSrcAccess = earlierWrites;
            transitions.push_back(transition);
        }
    }
}

template <typename Allocation> RHITextureSubResourceRange FullRange(const Allocation* resource)
{
    RHITextureSubResourceRange result{};

    if (resource->pTexture != nullptr)
    {
        result = resource->pTexture->GetSubResourceRange();
    }
    else
    {
        RHITextureSubResourceRange range = FormatIsDepthStencil(resource->texFormat.format) ?
            RHITextureSubResourceRange::DepthStencil() :
            FormatIsDepthOnly(resource->texFormat.format) ? RHITextureSubResourceRange::Depth() :
            FormatIsStencilOnly(resource->texFormat.format) ?
                                                            RHITextureSubResourceRange::Stencil() :
                                                            RHITextureSubResourceRange::Color();
        range.levelCount                 = resource->texFormat.mipmaps;
        range.layerCount                 = resource->texFormat.arrayLayers;
        result                           = range;
    }

    return result;
}
} // namespace

RDGTextureResourceState ResourceStateTracker::GetTextureState(const RHITexture* texture) const
{
    RDGTextureResourceState result{};

    if (texture == nullptr)
    {
        result = {};
    }
    else
    {
        std::unordered_map<uint64_t, RDGTextureResourceState>::const_iterator it =
            m_textureStates.find(texture->GetStableId());
        result = it == m_textureStates.end() ? RDGTextureResourceState{} : it->second;
    }

    return result;
}

RDGBufferResourceState ResourceStateTracker::GetBufferState(const RHIBuffer* buffer) const
{
    RDGBufferResourceState result{};

    if (buffer == nullptr)
    {
        result = {};
    }
    else
    {
        std::unordered_map<uint64_t, RDGBufferResourceState>::const_iterator it =
            m_bufferStates.find(buffer->GetStableId());
        result = it == m_bufferStates.end() ? RDGBufferResourceState{} : it->second;
    }

    return result;
}

void ResourceStateTracker::UpdateTextureState(const RHITexture* texture,
                                              RHIAccessMode mode,
                                              RHITextureUsage usage,
                                              BitField<RHIPipelineStageFlagBits> stages)
{
    if (texture != nullptr)
    {
        RemoveResourceState(texture->GetStableId(), true);
    }
    RDGTextureResourceState state{mode, usage, stages};
    state.writer = NewWriter(RHITextureUsageToAccessFlagBits(usage, mode), stages);
    SetTextureState(texture, state);
}

void ResourceStateTracker::SetTextureState(const RHITexture* texture,
                                           const RDGTextureResourceState& state)
{
    if (texture != nullptr)
    {
        ++m_revision;
        m_textureStates[texture->GetStableId()] = state;
    }
}

void ResourceStateTracker::UpdateBufferState(const RHIBuffer* buffer,
                                             RHIAccessMode mode,
                                             BitField<RHIBufferUsageFlagBits> usage,
                                             BitField<RHIPipelineStageFlagBits> stages)
{
    if (buffer != nullptr)
    {
        RemoveResourceState(buffer->GetStableId(), true);
    }
    RDGBufferResourceState state{mode, usage, stages};
    int64_t accesses = 0;

    for (uint32_t bit = 0; bit < 9; ++bit)
    {
        if (int64_t(usage) & (1 << bit))
        {
            accesses |=
                int64_t(RHIBufferUsageToAccessFlagBits(static_cast<RHIBufferUsage>(bit + 1), mode));
        }
    }

    state.writer = NewWriter(accesses, stages);
    SetBufferState(buffer, state);
}

void ResourceStateTracker::SetBufferState(const RHIBuffer* buffer,
                                          const RDGBufferResourceState& state)
{
    if (buffer != nullptr)
    {
        ++m_revision;
        m_bufferStates[buffer->GetStableId()] = state;
    }
}

RDGResourceContent ResourceStateTracker::GetContents(const RHIResource* resource) const
{
    RDGResourceContent result{};

    if (resource == nullptr)
    {
        result = {};
    }
    else
    {
        const std::unordered_map<uint64_t, RDGResourceContent>::const_iterator found =
            m_contents.find(resource->GetStableId());
        result = found == m_contents.end() ? RDGResourceContent{} : found->second;
    }

    return result;
}

void ResourceStateTracker::RemoveResourceState(uint64_t resourceId, bool invalidatePlans)
{
    size_t removed = m_textureStates.erase(resourceId);
    removed += m_bufferStates.erase(resourceId);
    removed += m_contents.erase(resourceId);
    if (removed != 0 || invalidatePlans)
    {
        ++m_revision;
    }
    if (m_metrics != nullptr)
    {
        m_metrics->ForgetResource(resourceId);
    }
}

void ResourceStateTracker::SetContents(const RHIResource* resource,
                                       const RDGResourceContent& contents)
{
    if (resource != nullptr)
    {
        ++m_revision;
        m_contents[resource->GetStableId()] = contents;
    }
}

bool RDGExecutor::PrepareExecution(RenderGraph* graph, ExecutionPlan& plan)
{
    bool result{};

    plan.consumed = true;

    if ((!(graph == nullptr || !graph->m_result)) &&
        (!(!graph->Check(!graph->m_inExecution, RDGErrorCode::eLifecycle,
                         "Cannot execute a graph recursively") ||
           !graph->Check(
               graph->m_executorIdentity == 0 || graph->m_executorIdentity == m_identity,
               RDGErrorCode::eLifecycle,
               "A graph and its pool must execute through the same resource-state tracker"))))
    {
        plan.graph             = graph;
        plan.executorIdentity  = m_identity;
        plan.buildGeneration   = graph->m_buildGeneration;
        plan.prepareCPUUs      = 0;
        plan.passTimings       = {};
        plan.preparationPasses = 0;
        plan.precompiled       = graph->m_executionState == RDGExecutionState::eCompiled;
        result                 = BuildExecutionPlan(plan);
    }

    return result;
}

bool RDGExecutor::BuildExecutionPlan(ExecutionPlan& plan)
{
    bool result{};

    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

    if (!CompileGraph(plan.graph))
    {
        plan.consumed = true;
        result        = false;
    }
    else
    {
        plan.transfer          = plan.graph->CanExecuteOnTransferQueue(m_resourceStateTracker);
        plan.stateRevision     = m_resourceStateTracker.GetRevision();
        plan.preparationSerial = plan.graph->m_preparationSerial;
        plan.prepareCPUUs +=
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
                .count();
        plan.passTimings.Accumulate(plan.graph->m_passCompileTimings);
        ++plan.preparationPasses;
        plan.consumed = false;
        result        = true;
    }

    return result;
}

bool RDGExecutor::CheckExecutionPlan(const ExecutionPlan& plan)
{
    return plan.graph != nullptr &&
        plan.graph->Check(!plan.consumed && plan.executorIdentity == m_identity &&
                              plan.buildGeneration == plan.graph->m_buildGeneration &&
                              plan.graph->m_executionState == RDGExecutionState::eCompiled &&
                              !plan.graph->m_inExecution &&
                              (plan.graph->m_executorIdentity == 0 ||
                               plan.graph->m_executorIdentity == m_identity),
                          RDGErrorCode::eLifecycle,
                          "Execution plan is consumed, stale, or belongs to another executor");
}

bool RDGExecutor::RefreshExecution(ExecutionPlan& plan)
{
    bool result{};

    if (CheckExecutionPlan(plan))
    {
        // Upload flushing can execute another graph and change both contents and initial hazards.
        // Another Prepare of this same graph can also replace its attached barriers.
        if (plan.stateRevision != m_resourceStateTracker.GetRevision() ||
            plan.preparationSerial != plan.graph->m_preparationSerial)
        {
            result = BuildExecutionPlan(plan);
        }
        else
        {
            result = true;
        }
    }

    return result;
}

bool RDGExecutor::Execute(RenderGraph* graph, RHICommandList* cmdList)
{
    bool result{};

    if (!(graph == nullptr || !graph->m_result))
    {
        if (cmdList == nullptr)
        {
            result = graph->Fail(RDGErrorCode::eLifecycle, "RDG execution requires a command list");
        }
        else
        {
            ExecutionPlan plan;
            result = PrepareExecution(graph, plan) && ExecutePrepared(plan, cmdList);
        }
    }

    return result;
}

bool RDGExecutor::ExecutePrepared(ExecutionPlan& plan,
                                  RHICommandList* cmdList,
                                  const std::function<RHISubmissionResult()>& submit,
                                  bool deferPublication)
{
    struct ExecutionScope
    {
        bool& executing;
        const bool previous;

        explicit ExecutionScope(bool& value) : executing(value), previous(value)
        {
            executing = true;
        }

        ~ExecutionScope()
        {
            executing = previous;
        }
    } scope(m_executing);
    bool result{};

    if (CheckExecutionPlan(plan))
    {
        plan.consumed      = true;
        RenderGraph* graph = plan.graph;
        const std::chrono::steady_clock::time_point validationStart =
            std::chrono::steady_clock::now();

        if (!graph->Check(cmdList != nullptr, RDGErrorCode::eLifecycle,
                          "RDG execution requires a command list") ||
            !graph->Check(
                plan.stateRevision == m_resourceStateTracker.GetRevision() &&
                    plan.preparationSerial == graph->m_preparationSerial,
                RDGErrorCode::eLifecycle,
                "Execution state changed after queue selection; refresh the execution plan") ||
            !graph->ValidateShaderIdentities())
        {
            graph->ReleaseCompiledShaderPasses();
            graph->m_resourceManager.ReleaseTransientResources();
            result = false;
        }
        else
        {
            plan.prepareCPUUs += std::chrono::duration<double, std::micro>(
                                     std::chrono::steady_clock::now() - validationStart)
                                     .count();
            const RHICommandListBase::CommandCheckpoint checkpoint =
                cmdList->GetCommandCheckpoint();
            graph->m_executorIdentity = m_identity;
            // Record into private state. Upload submissions before this checkpoint remain committed.
            ResourceStateTracker nextTracker = m_resourceStateTracker;
            RDGMetrics previousMetrics       = m_metrics;
            const bool observe               = m_metrics.Begin(*graph, plan.precompiled);

            for (RDGResult const& warning : graph->m_pendingContentWarnings)
            {
                LOGW("RDG [{}]: {}", uint32_t(warning.code), warning.message);
            }

            // Prepare is read-only with respect to persistent state. Apply explicit external states
            // only while recording, under the executor's rollback checkpoint.
            for (const RDGResourceManager::Allocation* resource :
                 graph->m_resourceManager.m_resources)
            {
                if (!resource->hasInitialState || resource->liveAccessCount == 0)
                {
                    continue;
                }

                if (resource->type == RDGResourceType::eTexture)
                {
                    const RDGTextureImportState& state = resource->initialTextureState;
                    nextTracker.UpdateTextureState(resource->pTexture, state.accessMode,
                                                   state.usage, state.stages);
                }
                else
                {
                    const RDGBufferImportState& state = resource->initialBufferState;
                    nextTracker.UpdateBufferState(resource->pBuffer, state.accessMode, state.usage,
                                                  state.stages);
                }
            }

            if (observe)
            {
                m_metrics.Compiled(*graph, plan.prepareCPUUs, plan.preparationPasses,
                                   plan.passTimings);
                graph->m_activeMetrics = &m_metrics;
            }

            graph->m_inExecution                 = true;
            bool success                         = graph->Execute(cmdList, nextTracker);
            graph->m_inExecution                 = false;
            graph->m_pCmdList                    = nullptr;
            graph->m_activeMetrics               = nullptr;
            RHISubmissionResult submissionResult = RHISubmissionResult::eSuccess;
            double submissionCPUUs               = 0;

            if (success && plan.stateRevision != m_resourceStateTracker.GetRevision())
            {
                success = graph->Fail(RDGErrorCode::eLifecycle,
                                      "Persistent resource state changed during command recording");
            }

            if (success && submit)
            {
                const std::chrono::steady_clock::time_point start =
                    std::chrono::steady_clock::now();
                submissionResult = submit();
                submissionCPUUs  = std::chrono::duration<double, std::micro>(
                                       std::chrono::steady_clock::now() - start)
                                       .count();

                if (submissionResult != RHISubmissionResult::eSuccess)
                {
                    success = graph->Fail(
                        RDGErrorCode::eSubmission,
                        submissionResult == RHISubmissionResult::eRejected ?
                            "GPU submission rejected; recorded state was not committed" :
                            "GPU submission or completion failed; recreate the device before continuing");
                }
            }

            if (success)
            {
                graph->CommitContents(nextTracker);
                m_resourceStateTracker = std::move(nextTracker);

                if (submit && !deferPublication)
                {
                    graph->m_resourceManager.PublishExtractions(m_pRenderDevice);
                }

                if (observe)
                {
                    m_metrics.End(submissionCPUUs);
                }

                result = true;
            }
            else
            {
                cmdList->RollbackCommands(checkpoint);
                m_metrics = std::move(previousMetrics);

                if (submissionResult == RHISubmissionResult::eFatal)
                {
                    // Some commands may have executed. Neither the pre-recording nor final state is reliable.
                    for (const RDGResourceManager::Allocation* resource :
                         graph->m_resourceManager.m_resources)
                    {
                        if (resource->liveAccessCount == 0)
                        {
                            continue;
                        }

                        const RHIResource* rhiResource =
                            resource->type == RDGResourceType::eTexture ?
                            static_cast<const RHIResource*>(resource->pTexture) :
                            static_cast<const RHIResource*>(resource->pBuffer);
                        if (rhiResource != nullptr)
                        {
                            m_resourceStateTracker.RemoveResourceState(rhiResource->GetStableId(),
                                                                       true);
                        }
                    }
                }

                graph->ReleaseCompiledShaderPasses();
                graph->m_resourceManager.ReleaseTransientResources();
                result = false;
            }
        }
    }

    return result;
}

bool RDGExecutor::Prepare(RenderGraph* graph)
{
    return CompileGraph(graph);
}

bool RDGExecutor::CompileGraph(RenderGraph* graph)
{
    bool result{};

    if (!(graph == nullptr || !graph->m_result))
    {
        if (graph->m_inExecution)
        {
            result = graph->Fail(RDGErrorCode::eLifecycle, "Cannot compile an executing graph");
        }
        else
        {
            graph->m_passCompileTimings = {};
            graph->m_passCompileTimings.enabled =
                m_metrics.GetOptions().logging.enabled && m_metrics.GetOptions().preparationTimings;
            const PipelineCacheMetrics previousPipelines = m_pRenderDevice ?
                m_pRenderDevice->GetPipelineCacheMetrics() :
                PipelineCacheMetrics{};
            const bool compiled                          = CompileGraphInternal(graph);

            if (m_pRenderDevice)
            {
                graph->m_passCompileTimings.pipelines =
                    m_pRenderDevice->GetPipelineCacheMetrics().Since(previousPipelines);
            }

            if (compiled)
            {
                ++graph->m_preparationSerial;
                result = true;
            }
            else
            {
                graph->ReleaseCompiledShaderPasses();
                graph->m_resourceManager.ReleaseTransientResources();
                result = false;
            }
        }
    }

    return result;
}

bool RDGExecutor::CompileGraphInternal(RenderGraph* graph)
{
    bool valid =
        graph->Check(graph->m_executionState == RDGExecutionState::eRecorded ||
                         graph->m_executionState == RDGExecutionState::eCompiled,
                     RDGErrorCode::eLifecycle, "RenderGraph must be ended before compilation") &&
        graph->Check(
            m_pRenderDevice != nullptr &&
                (graph->m_pRenderDevice == nullptr || graph->m_pRenderDevice == m_pRenderDevice),
            RDGErrorCode::eLifecycle,
            "Graph execution requires its original RenderDevice for resource retirement");
    const bool alreadyCompiled = graph->m_executionState == RDGExecutionState::eCompiled;

    // Validate descriptions before physical allocations or pipeline work.
    if (valid)
    {
        valid =
            std::all_of(graph->m_pendingGfxPassDescs.begin(), graph->m_pendingGfxPassDescs.end(),
                        [graph](const RDGGraphicsPassDesc& desc) {
                            return graph->ValidatePassDescription(desc, true) != nullptr;
                        }) &&
            std::all_of(graph->m_pendingComputePassDescs.begin(),
                        graph->m_pendingComputePassDescs.end(),
                        [graph](const RDGComputePassDesc& desc) {
                            return graph->ValidatePassDescription(desc, false) != nullptr;
                        });
    }

    if (valid && !alreadyCompiled)
    {
        graph->m_pRenderDevice = m_pRenderDevice;
        valid                  = graph->SortNodesByVersion();

        if (valid)
        {
            graph->DetermineLiveness();
            graph->BuildCompiledNodeList();
            valid = graph->ValidateContents(m_resourceStateTracker);
        }

        if (valid)
        {
            const RDGResult materialized =
                graph->m_resourceManager.MaterializeTransientResources(m_pRenderDevice);
            valid = static_cast<bool>(materialized) ||
                graph->Fail(materialized.code, materialized.message);
        }

        if (valid)
        {
            valid = std::all_of(graph->m_resourceManager.m_resources.begin(),
                                graph->m_resourceManager.m_resources.end(),
                                [graph](const RDGResourceManager::Allocation* resource) {
                                    return graph->Check(resource->liveAccessCount == 0 ||
                                                            PhysicalResource(resource) != nullptr,
                                                        RDGErrorCode::eAllocation,
                                                        "Failed to materialize resource '" +
                                                            resource->name.ToString() + "'");
                                });
        }

        if (valid)
        {
            for (RDGNodeBase* base : graph->m_nodes)
            {
                RDGPassNode* node = static_cast<RDGPassNode*>(base);

                if (!node->live || node->pCompiledPass != nullptr)
                {
                    continue;
                }

                RDGPassCompiler& compiler = m_pRenderDevice->GetRDGPassCompiler(nullptr);
                compiler.SetRenderGraph(graph);

                if (node->type == RDGNodeType::eGraphicsPass)
                {
                    RDGGraphicsPassDesc& desc = graph->m_pendingGfxPassDescs[node->passDescIdx];

                    for (uint32_t i = 0; i < desc.colorOutputCount; ++i)
                    {
                        desc.colorOutputs[i].pTexture =
                            graph->m_resourceManager.Resolve(desc.colorOutputs[i].texture)
                                ->pTexture;
                    }

                    if (desc.HasDepthStencilOutput())
                    {
                        desc.depthStencilOutput.pTexture =
                            graph->m_resourceManager.Resolve(desc.depthStencilOutput.texture)
                                ->pTexture;
                    }

                    node->pCompiledPass = compiler.CompileGraphicsPass(desc);
                }
                else
                {
                    node->pCompiledPass = compiler.CompileComputePass(
                        graph->m_pendingComputePassDescs[node->passDescIdx]);
                }

                if (node->pCompiledPass == nullptr)
                {
                    valid = false;
                    break;
                }
            }
        }

        valid = valid && graph->ValidateCompiledGraph();

        if (valid)
        {
            graph->m_executionState = RDGExecutionState::eCompiled;
        }
    }

    return valid && (!alreadyCompiled || graph->ValidateContents(m_resourceStateTracker)) &&
        AttachGraphBarriers(graph);
}

bool RDGExecutor::AttachGraphBarriers(RenderGraph* graph)
{
    bool valid = true;

    m_frameBufferStates.clear();
    m_frameTextureStates.clear();
    graph->m_compileStats.dependencyBarrierCount = 0;

    for (RDGCompiledNode& compiled : graph->m_compiledNodes)
    {
        compiled.prologueSrcStages.Clear();
        compiled.prologueDstStages.Clear();
        compiled.initialBarrierCount = 0;
        compiled.initialResourceAccesses.clear();
        compiled.prologueBufferTransitions.clear();
        compiled.prologueTextureTransitions.clear();
        const RDGNodeBase* node = graph->GetNodeBaseById(compiled.nodeId);

        for (uint32_t i = 0; i < node->accessCount; ++i)
        {
            RDGAccess const& access = graph->m_accesses[node->accessOffset + i];
            const RDGResourceManager::Allocation* resource =
                graph->m_resourceManager.FindResourceByIdx(access.resourceId);
            valid =
                graph->Check(resource != nullptr && PhysicalResource(resource) != nullptr,
                             RDGErrorCode::eAllocation, "RDG resource has not been materialized");

            if (valid)
            {
                bool first   = false;
                bool barrier = false;

                if (resource->type == RDGResourceType::eBuffer)
                {
                    const std::pair<FlatHashMap<RHIBuffer*, RDGBufferResourceState>::iterator, bool>
                        itResult = m_frameBufferStates.try_emplace(resource->pBuffer);
                    FlatHashMap<RHIBuffer*, RDGBufferResourceState>::iterator it = itResult.first;
                    bool inserted                                                = itResult.second;

                    if (inserted)
                    {
                        it->second = InitialBufferState(resource, m_resourceStateTracker);
                    }

                    first                                  = inserted;
                    RDGBufferResourceState const& previous = it->second;
                    barrier                                = NeedsResourceBarrier(previous, access);

                    if (barrier)
                    {
                        compiled.prologueSrcStages.SetFlag(previous.pipelineStages);
                        compiled.prologueSrcStages.SetFlag(previous.writer.stages);
                        AddBufferTransitions(compiled.prologueBufferTransitions, resource->pBuffer,
                                             previous.accessMode, access.accessMode, previous.usage,
                                             access.bufferUsage, previous.writer.access);
                    }
                }
                else
                {
                    const std::pair<FlatHashMap<RHITexture*, RDGTextureResourceState>::iterator,
                                    bool>
                        itResult = m_frameTextureStates.try_emplace(resource->pTexture);
                    FlatHashMap<RHITexture*, RDGTextureResourceState>::iterator it = itResult.first;
                    bool inserted = itResult.second;

                    if (inserted)
                    {
                        it->second = InitialTextureState(resource, m_resourceStateTracker);
                    }

                    first                                   = inserted;
                    RDGTextureResourceState const& previous = it->second;
                    barrier = NeedsResourceBarrier(previous, access);

                    if (barrier)
                    {
                        compiled.prologueSrcStages.SetFlag(previous.pipelineStages);
                        compiled.prologueSrcStages.SetFlag(previous.writer.stages);
                        RHITextureTransition transition{};
                        transition.pTexture            = resource->pTexture;
                        transition.oldAccessMode       = previous.accessMode;
                        transition.newAccessMode       = access.accessMode;
                        transition.oldUsage            = previous.usage;
                        transition.newUsage            = access.textureUsage;
                        transition.subResourceRange    = FullRange(resource);
                        transition.additionalSrcAccess = previous.writer.access;
                        compiled.prologueTextureTransitions.push_back(transition);
                    }
                }

                if (first)
                {
                    compiled.initialResourceAccesses.push_back(access);
                }

                if (barrier)
                {
                    compiled.prologueDstStages.SetFlag(access.pipelineStages);

                    if (first)
                    {
                        ++compiled.initialBarrierCount;
                    }
                    else
                    {
                        ++graph->m_compileStats.dependencyBarrierCount;
                    }
                }
            }

            if (!valid)
            {
                break;
            }
        }

        // Every transition in this batch receives the same destination stage mask. Record its
        // actual memory coverage after all resources have contributed their destination stages.
        if (valid)
        {
            for (uint32_t i = 0; i < node->accessCount; ++i)
            {
                RDGAccess const& access = graph->m_accesses[node->accessOffset + i];
                const RDGResourceManager::Allocation* resource =
                    graph->m_resourceManager.FindResourceByIdx(access.resourceId);

                if (resource->type == RDGResourceType::eBuffer)
                {
                    RDGBufferResourceState& state =
                        m_frameBufferStates.find(resource->pBuffer)->second;
                    state = AdvanceState(state, access, compiled.prologueDstStages);
                }
                else
                {
                    RDGTextureResourceState& state =
                        m_frameTextureStates.find(resource->pTexture)->second;
                    state = AdvanceState(state, access, compiled.prologueDstStages);
                }
            }
        }

        if (!valid)
        {
            break;
        }
    }

    return valid;
}

bool RenderGraph::Fail(RDGErrorCode code, const std::string& message)
{
    if (m_result)
    {
        m_result = {code, message};
        LOGE("RDG '{}' [{}]: {}", m_rdgTag.CStr(), static_cast<uint32_t>(code), message);
    }

    m_executionState = RDGExecutionState::eInvalid;

    return false;
}

bool RenderGraph::Check(bool condition, RDGErrorCode code, const std::string& message)
{
    bool result{};

    if (m_result)
    {
        result = condition || Fail(code, message);
    }

    return result;
}

bool RenderGraph::CheckRecorder(const RDGPassNode* node, uint64_t generation)
{
    bool result{};

    if (m_result)
    {
        // Check generation before touching an arena pointer retained by a recorder.
        if (m_executionState != RDGExecutionState::eBuilding || generation != m_buildGeneration ||
            node == nullptr)
        {
            result =
                Fail(RDGErrorCode::eLifecycle, "Recorder requires its original building graph");
        }
        else
        {
            result = true;
        }
    }

    return result;
}

bool RenderGraph::ValidateTextureRange(const RDGResourceManager::Allocation* resource,
                                       const RHITextureSubResourceRange& range)
{
    bool result{};

    if (Check(resource != nullptr && resource->type == RDGResourceType::eTexture,
              RDGErrorCode::eRange, "Invalid texture resource"))
    {
        const TextureFormat& format           = resource->texFormat;
        const RHITextureSubResourceRange full = FullRange(resource);

        if (Check(!range.aspect.IsEmpty() && (int64_t(range.aspect) & ~int64_t(full.aspect)) == 0 &&
                      range.levelCount > 0 && range.baseMipLevel < format.mipmaps &&
                      range.levelCount <= format.mipmaps - range.baseMipLevel &&
                      range.layerCount > 0 && range.baseArrayLayer < format.arrayLayers &&
                      range.layerCount <= format.arrayLayers - range.baseArrayLayer,
                  RDGErrorCode::eRange,
                  "Invalid subresource range for '" + resource->name.ToString() + "'"))
        {
            result = true;
        }
    }

    return result;
}

bool RenderGraph::ValidateShaderIdentity(const RDGPassDescBase& desc)
{
    bool valid = true;

    const ShaderProgram* program =
        ShaderProgramManager::GetInstance().RequestShaderProgram(desc.shaderProgramName);

    if (program == nullptr || program->GetShader() == nullptr ||
        desc.shaderIdentity != program->GetShader()->GetStableId())
    {
        valid = Fail(RDGErrorCode::eShader,
                     "Pass '" + desc.passTag.ToString() + "': Shader changed; rebuild the graph");
    }

    return valid;
}

bool RenderGraph::ValidateShaderIdentities()
{
    bool valid = true;

    for (const RDGGraphicsPassDesc& desc : m_pendingGfxPassDescs)
    {
        valid = ValidateShaderIdentity(desc);

        if (!valid)
        {
            break;
        }
    }

    if (valid)
    {
        for (const RDGComputePassDesc& desc : m_pendingComputePassDescs)
        {
            valid = ValidateShaderIdentity(desc);

            if (!valid)
            {
                break;
            }
        }
    }

    return valid;
}

struct RenderGraph::PassBindingValidator
{
    RenderGraph& graph;
    ShaderProgram* program;
    const RDGPassDescBase& desc;
    const std::string& prefix;
    std::unordered_set<NameID> names;

    bool Check(bool condition, RDGErrorCode code, const std::string& message)
    {
        return graph.Check(condition, code, message);
    }

    const RHIShaderResourceDescriptor* Descriptor(NameID name,
                                                  RHIShaderResourceType type,
                                                  uint32_t count)
    {
        const RHIShaderResourceDescriptor* result = program->GetShaderResourceDescriptor(name);
        const bool compatible =
            Check(result != nullptr && result->type == type, RDGErrorCode::eBinding,
                  prefix + "Unknown or incompatible binding '" + name.ToString() + "'") &&
            Check(names.insert(name).second, RDGErrorCode::eBinding,
                  prefix + "Duplicate binding '" + name.ToString() + "'") &&
            Check(count > 0 &&
                      (type == RHIShaderResourceType::eUniformBuffer || result->bindless ||
                       (result->arraySize > 0 && count <= result->arraySize)),
                  RDGErrorCode::eBinding,
                  prefix + "Invalid array count for '" + name.ToString() + "'");

        return compatible ? result : nullptr;
    }

    bool Slice(RDGBindingSlice slice, size_t size)
    {
        return Check(slice.count > 0 && slice.offset <= size && slice.count <= size - slice.offset,
                     RDGErrorCode::eBinding, prefix + "Invalid binding slice");
    }

    bool ValidateValues()
    {
        bool allValid = true;

        for (const RDGValueBinding& binding : desc.valueBindings)
        {
            const RHIShaderResourceDescriptor* srd =
                Descriptor(binding.glslName, RHIShaderResourceType::eUniformBuffer, 1);
            allValid = srd != nullptr && Slice(binding.bytes, desc.valueByteStorage.size()) &&
                Check(srd->blockSize == 0 || binding.bytes.count <= srd->blockSize,
                      RDGErrorCode::eBinding,
                      prefix + "Value exceeds uniform block '" + binding.glslName.ToString() + "'");

            if (!allValid)
            {
                break;
            }
        }

        return allValid;
    }

    bool ValidateBuffers()
    {
        bool allValid = true;

        for (const RDGBufferBinding& binding : desc.UAVBufferBindings)
        {
            const RHIShaderResourceDescriptor* srd = Descriptor(
                binding.glslName, RHIShaderResourceType::eStorageBuffer, binding.buffers.count);
            bool compatible = srd != nullptr &&
                Check(ValidContentGuarantee(*srd, binding.contents), RDGErrorCode::eBinding,
                      prefix + "Storage-buffer content guarantee conflicts with reflection") &&
                Slice(binding.buffers, desc.bufferStorage.size());

            for (uint32_t i = 0; compatible && i < binding.buffers.count; ++i)
            {
                compatible = Check(desc.bufferStorage[binding.buffers.offset + i] != nullptr,
                                   RDGErrorCode::eBinding,
                                   prefix + "Null buffer in '" + binding.glslName.ToString() + "'");
            }

            allValid = compatible;

            if (!allValid)
            {
                break;
            }
        }

        return allValid;
    }

    bool ValidateTextures(const HeapVector<RDGTextureBinding>& bindings, RHIShaderResourceType type)
    {
        bool allValid = true;

        for (const RDGTextureBinding& binding : bindings)
        {
            const RHIShaderResourceDescriptor* srd =
                Descriptor(binding.glslName, type, binding.views.count);
            bool compatible = srd != nullptr &&
                Check(ValidContentGuarantee(*srd, binding.contents), RDGErrorCode::eBinding,
                      prefix + "Texture content guarantee conflicts with reflection") &&
                Slice(binding.views, desc.textureViewStorage.size());

            for (uint32_t i = 0; compatible && i < binding.views.count; ++i)
            {
                RHITextureView* view = desc.textureViewStorage[binding.views.offset + i];
                compatible =
                    Check(view != nullptr && view->GetTexture() != nullptr, RDGErrorCode::eBinding,
                          prefix + "Null texture view in '" + binding.glslName.ToString() + "'");

                if (compatible)
                {
                    const RHITextureSubResourceRange& range = view->GetSubResourceRange();
                    const RHITextureSubResourceRange& full =
                        view->GetTexture()->GetSubResourceRange();
                    compatible =
                        Check(!range.aspect.IsEmpty() &&
                                  (int64_t(range.aspect) & ~int64_t(full.aspect)) == 0 &&
                                  range.levelCount > 0 && range.baseMipLevel < full.levelCount &&
                                  range.levelCount <= full.levelCount - range.baseMipLevel &&
                                  range.layerCount > 0 && range.baseArrayLayer < full.layerCount &&
                                  range.layerCount <= full.layerCount - range.baseArrayLayer,
                              RDGErrorCode::eRange,
                              prefix + "Invalid texture view range in '" +
                                  binding.glslName.ToString() + "'");
                }
            }

            allValid = compatible;

            if (!allValid)
            {
                break;
            }
        }

        return allValid;
    }

    bool ValidateSamplers()
    {
        bool allValid = true;
        for (const RDGSamplerBinding& binding : desc.samplerBindings)
        {
            const RHIShaderResourceDescriptor* srd =
                Descriptor(binding.glslName, RHIShaderResourceType::eSampler, 1);

            allValid = srd != nullptr &&
                Check(binding.pSampler != nullptr, RDGErrorCode::eBinding,
                      prefix + "Null sampler in '" + binding.glslName.ToString() + "'");

            if (!allValid)
            {
                break;
            }
        }

        return allValid;
    }

    bool ValidateResources()
    {
        bool allValid = true;

        for (const RDGResourceBinding& binding : desc.resourceBindings)
        {
            bool compatible =
                Check(binding.resourceType > RDGBindingType::eNone &&
                          binding.resourceType < RDGBindingType::eMax,
                      RDGErrorCode::eBinding, prefix + "Invalid logical binding type") &&
                Slice(binding.resources, desc.resourceStorage.size());

            if (compatible)
            {
                const RHIShaderResourceType type = LogicalBindingType(binding.resourceType);
                const RHIShaderResourceDescriptor* srd =
                    Descriptor(binding.glslName, type, binding.resources.count);
                const bool buffer = type == RHIShaderResourceType::eStorageBuffer ||
                    type == RHIShaderResourceType::eUniformBuffer;
                compatible = srd != nullptr &&
                    Check(ValidContentGuarantee(*srd, binding.contents), RDGErrorCode::eBinding,
                          prefix + "Logical content guarantee conflicts with reflection");
                compatible = compatible &&
                    (type != RHIShaderResourceType::eUniformBuffer ||
                     Check(srd->arraySize >= binding.resources.count, RDGErrorCode::eRange,
                           prefix + "Logical uniform array exceeds reflected count"));

                for (uint32_t i = 0; compatible && i < binding.resources.count; ++i)
                {
                    const RDGBoundResource& element =
                        desc.resourceStorage[binding.resources.offset + i];
                    const RDGResourceManager::Allocation* resource = nullptr;

                    if (element.resource)
                    {
                        resource   = buffer ?
                            graph.m_resourceManager.Resolve(element.resource) :
                            graph.m_resourceManager.ResolveView(element.resource, element.view);
                        compatible = resource != nullptr;
                    }
                    else
                    {
                        const HashMap<NameID, RDGTransientOutput>::iterator found =
                            graph.m_transientRTMap.find(binding.producerOutputTag);
                        compatible = Check(binding.resources.count == 1 &&
                                               found != graph.m_transientRTMap.end(),
                                           RDGErrorCode::eBinding,
                                           prefix + "No preceding producer for '" +
                                               binding.producerOutputTag.ToString() + "'");

                        if (compatible)
                        {
                            resource = found->second.pResource;
                        }
                    }

                    compatible = compatible &&
                        Check(resource->type ==
                                      (buffer ? RDGResourceType::eBuffer :
                                                RDGResourceType::eTexture) &&
                                  (!buffer || !element.view.range),
                              RDGErrorCode::eBinding,
                              prefix + "Logical binding has an incompatible resource or view type");
                    compatible = compatible &&
                        (type != RHIShaderResourceType::eUniformBuffer ||
                         Check(resource->bufferSize >= srd->blockSize, RDGErrorCode::eRange,
                               prefix +
                                   "Logical uniform buffer is smaller than its reflected block"));
                }
            }

            allValid = compatible;

            if (!allValid)
            {
                break;
            }
        }

        return allValid;
    }

    bool ValidateIndirectBuffers()
    {
        bool allValid = true;

        for (const RHIBuffer* buffer : desc.indirectBuffers)
        {
            allValid =
                Check(buffer != nullptr, RDGErrorCode::eBinding, prefix + "Null indirect buffer");

            if (!allValid)
            {
                break;
            }
        }

        return allValid;
    }
};

ShaderProgram* RenderGraph::ValidatePassDescription(const RDGPassDescBase& desc, bool graphics)
{
    const std::string prefix = "Pass '" + desc.passTag.ToString() + "': ";
    bool valid               = Check(bool(desc.validationResult), desc.validationResult.code,
                                     prefix + desc.validationResult.message);
    ShaderProgram* program   = valid ?
        ShaderProgramManager::GetInstance().RequestShaderProgram(desc.shaderProgramName) :
        nullptr;
    valid                    = valid &&
        Check(program != nullptr && program->GetShader() != nullptr, RDGErrorCode::eShader,
              prefix + "Unknown or uninitialized shader '" + desc.shaderProgramName.ToString() +
                  "'");

    if (valid)
    {
        valid = Check(desc.shaderIdentity == 0 ||
                          desc.shaderIdentity == program->GetShader()->GetStableId(),
                      RDGErrorCode::eShader, prefix + "Shader changed; rebuild the graph");
    }

    if (valid)
    {
        const BitField<RHIShaderStageFlagBits> stages =
            program->GetShader()->GetCreateInfo().stageFlags;
        valid = Check(
            graphics ? !stages.HasFlag(RHIShaderStageFlagBits::eCompute) :
                       (int64_t(stages) & ~int64_t(RHIShaderStageFlagBits::eCompute)) == 0,
            RDGErrorCode::eShader, prefix + "Shader stages are incompatible with the pass type");
    }

    if (valid)
    {
        PassBindingValidator validator{*this, program, desc, prefix, {}};
        valid = validator.ValidateValues() && validator.ValidateBuffers() &&
            validator.ValidateTextures(desc.sampledTexBindings,
                                       RHIShaderResourceType::eSamplerWithTexture) &&
            validator.ValidateTextures(desc.separateTexBindings, RHIShaderResourceType::eTexture) &&
            validator.ValidateSamplers() &&
            validator.ValidateTextures(desc.UAVTexBindings, RHIShaderResourceType::eImage) &&
            validator.ValidateResources() && validator.ValidateIndirectBuffers();
    }

    return valid ? program : nullptr;
}

template <typename Output> bool RenderGraph::ResolveAttachment(Output& output)
{
    bool valid = true;

    if (output.texture)
    {
        const RDGResourceManager::Allocation* resource = m_resourceManager.Resolve(output.texture);
        valid                                          = (resource != nullptr);
        valid                                          = valid &&
            (Check(resource->type == RDGResourceType::eTexture &&
                       resource->texFormat.mipmaps == 1 && resource->texFormat.arrayLayers == 1 &&
                       resource->texFormat.depth == 1,
                   RDGErrorCode::eAttachment,
                   "Attachments require a single mip and layer in the current backend"));

        if (valid)
        {
            output.format = resource->texFormat.format;
            output.width  = resource->texFormat.width;
            output.height = resource->texFormat.height;
        }
    }

    return valid;
}

bool RenderGraph::ResolveAttachments(RDGGraphicsPassDesc& desc)
{
    bool valid = true;

    valid = Check(desc.colorOutputCount <= MAX_NUM_COLOR_ATTACHMENTS, RDGErrorCode::eAttachment,
                  "Too many color attachments");

    if (valid)
    {
        for (uint32_t i = 0; i < desc.colorOutputCount; ++i)
        {
            valid = ResolveAttachment(desc.colorOutputs[i]);

            if (!valid)
            {
                break;
            }
        }

        if (valid)
        {
            valid = !desc.HasDepthStencilOutput() || ResolveAttachment(desc.depthStencilOutput);
        }
    }

    return valid;
}

template <typename Output> bool RenderGraph::ValidateAttachment(const RDGGraphicsPassDesc& desc,
                                                                const std::string& prefix,
                                                                std::unordered_set<NameID>& tags,
                                                                const Output& out,
                                                                bool depth,
                                                                uint32_t slot)
{
    bool valid = true;

    valid = Check(out.format != DataFormat::eUndefined && out.width > 0 && out.height > 0,
                  RDGErrorCode::eAttachment, prefix + "Invalid attachment format or extent");

    if (valid)
    {
        const RDGResourceManager::Allocation* attachment =
            out.texture ? m_resourceManager.Resolve(out.texture) : nullptr;
        const RHITexture* physical = out.pTexture;
        valid = Check(physical == nullptr ||
                          (physical->GetNumMipmaps() == 1 && physical->GetArrayLayers() == 1 &&
                           physical->GetDepth() == 1 &&
                           physical->GetBaseInfo().samples ==
                               desc.pipelineStates.multiSampleState.sampleCount),
                      RDGErrorCode::eAttachment,
                      prefix + "Attachment shape or samples do not match the pass");
        valid = valid &&
            (Check(attachment == nullptr ||
                       attachment->texFormat.sampleCount ==
                           desc.pipelineStates.multiSampleState.sampleCount,
                   RDGErrorCode::eAttachment,
                   prefix + "Logical attachment samples do not match the pass"));

        if (valid)
        {
            const bool depthFormat = FormatIsDepthOnly(out.format) ||
                FormatIsDepthStencil(out.format) || FormatIsStencilOnly(out.format);
            valid = Check(depth == depthFormat, RDGErrorCode::eAttachment,
                          prefix + "Attachment format does not match its slot");
            valid = valid &&
                (Check((out.loadOp == RHIRenderTargetLoadOp::eClear ||
                        out.loadOp == RHIRenderTargetLoadOp::eLoad ||
                        out.loadOp == RHIRenderTargetLoadOp::eNone) &&
                           (out.storeOp == RHIRenderTargetStoreOp::eStore ||
                            out.storeOp == RHIRenderTargetStoreOp::eNone),
                       RDGErrorCode::eAttachment,
                       prefix + "Invalid attachment load/store operation"));
            valid = valid &&
                (Check(desc.renderArea.minX >= 0 && desc.renderArea.minY >= 0 &&
                           desc.renderArea.maxX >= 0 && desc.renderArea.maxY >= 0 &&
                           uint64_t(desc.renderArea.minX) + desc.renderArea.maxX <= out.width &&
                           uint64_t(desc.renderArea.minY) + desc.renderArea.maxY <= out.height,
                       RDGErrorCode::eAttachment, prefix + "Render area exceeds attachment"));

            if (valid)
            {
                if (out.loadOp == RHIRenderTargetLoadOp::eNone)
                {
                    const RHIGfxPipelineDepthStencilState& depthState =
                        desc.pipelineStates.depthStencilState;
                    bool readsDestination = depth &&
                        (depthState.enableDepthTest || depthState.enableStencilTest ||
                         depthState.enableDepthBoundsTest);

                    if (!depth)
                    {
                        readsDestination = desc.pipelineStates.colorBlendState.enableLogicOp ||
                            desc.pipelineStates.colorBlendState.attachments[slot].enableBlend;
                    }

                    valid = Check(
                        !readsDestination, RDGErrorCode::eUninitialized,
                        prefix +
                            "Attachment Load=None cannot supply destination reads from blending/depth/stencil tests");
                }

                if ((valid) && (!out.tag.IsNone()))
                {
                    valid = Check(tags.insert(out.tag).second, RDGErrorCode::eDuplicateTag,
                                  prefix + "Duplicate output tag '" + out.tag.ToString() + "'");

                    if (valid)
                    {
                        const std::unordered_map<NameID, RenderGraph::RDGTransientOutput>::iterator
                            existing = m_transientRTMap.find(out.tag);
                        // Repeated writes to the same imported attachment keep ordered-mutation semantics.
                        valid = Check(existing == m_transientRTMap.end() ||
                                          (out.pTexture != nullptr &&
                                           existing->second.pResource->imported &&
                                           existing->second.pResource->pTexture == out.pTexture),
                                      RDGErrorCode::eDuplicateTag,
                                      prefix + "Duplicate output tag '" + out.tag.ToString() + "'");
                    }
                }
            }
        }
    }

    return valid;
}

bool RenderGraph::ValidateGraphicsDescription(const RDGGraphicsPassDesc& desc)
{
    bool valid = true;

    const std::string prefix = "Pass '" + desc.passTag.ToString() + "': ";
    valid = Check(desc.colorOutputCount <= MAX_NUM_COLOR_ATTACHMENTS, RDGErrorCode::eAttachment,
                  prefix + "Too many color attachments");

    if (valid)
    {
        std::unordered_set<NameID> tags;

        for (uint32_t i = 0; i < MAX_NUM_COLOR_ATTACHMENTS; ++i)
        {
            valid =
                Check(desc.HasColorOutput(i) == (i < desc.colorOutputCount),
                      RDGErrorCode::eAttachment, prefix + "Color attachment count/mask mismatch");

            if ((valid) && (i < desc.colorOutputCount))
            {
                valid = ValidateAttachment(desc, prefix, tags, desc.colorOutputs[i], false, i);
            }

            if (!valid)
            {
                break;
            }
        }

        if (valid)
        {
            if (desc.HasDepthStencilOutput())
            {
                valid = ValidateAttachment(desc, prefix, tags, desc.depthStencilOutput, true, 0);
            }

            valid = valid &&
                (!((!Check(desc.vertexBuffers.empty() || desc.geometryBuffer.vertexBuffers.empty(),
                           RDGErrorCode::eBinding,
                           prefix + "Cannot mix logical and raw vertex binding lists") ||
                    !Check(!desc.indexBuffer || desc.geometryBuffer.pIndexBuffer == nullptr,
                           RDGErrorCode::eBinding, prefix + "Cannot specify two index buffers"))));
        }

        if (valid)
        {
            for (RDGBuffer const buffer : desc.vertexBuffers)
            {
                valid = (m_resourceManager.Resolve(buffer) != nullptr);

                if (!valid)
                {
                    break;
                }
            }
        }

        if (valid)
        {
            if (desc.indexBuffer)
            {
                const RDGResourceManager::Allocation* resource =
                    m_resourceManager.Resolve(desc.indexBuffer);
                valid = (resource != nullptr);

                if (valid)
                {
                    const uint32_t stride = desc.indexBufferFormat == DataFormat::eR16UInt ? 2 : 4;
                    valid = Check((desc.indexBufferFormat == DataFormat::eR16UInt ||
                                   desc.indexBufferFormat == DataFormat::eR32UInt) &&
                                      desc.indexBufferOffset % stride == 0 &&
                                      desc.indexBufferOffset < resource->bufferSize,
                                  RDGErrorCode::eRange,
                                  prefix + "Invalid logical index buffer format or offset");
                }
            }
        }

        if (valid)
        {
            for (RHIBuffer* buffer : desc.geometryBuffer.vertexBuffers)
            {
                valid =
                    Check(buffer != nullptr, RDGErrorCode::eBinding, prefix + "Null vertex buffer");

                if (!valid)
                {
                    break;
                }
            }

            if ((valid) && (desc.geometryBuffer.pIndexBuffer != nullptr))
            {
                const RHIGeometryBuffer& geometry = desc.geometryBuffer;
                const uint32_t stride = geometry.indexBufferFormat == DataFormat::eR16UInt ? 2 : 4;
                valid =
                    Check((geometry.indexBufferFormat == DataFormat::eR16UInt ||
                           geometry.indexBufferFormat == DataFormat::eR32UInt) &&
                              geometry.indexBufferOffset % stride == 0 &&
                              geometry.indexBufferOffset < geometry.pIndexBuffer->GetRequiredSize(),
                          RDGErrorCode::eRange, prefix + "Invalid index buffer format or offset");
            }
        }
    }

    return valid;
}

RDGShaderPassCmdRecorder RenderGraph::AddGraphicsPass(RDGGraphicsPassDesc desc)
{
    RDGPassNode* pRecordedNode = nullptr;
    bool valid                 = true;

    valid = static_cast<bool>(m_result);
    valid = valid &&
        (Check(m_executionState == RDGExecutionState::eBuilding, RDGErrorCode::eLifecycle,
               "AddGraphicsPass requires Begin"));

    if (valid)
    {
        ShaderProgram* shader = ValidatePassDescription(desc, true);
        valid                 = (shader != nullptr);
        valid = valid && (!((!ResolveAttachments(desc) || !ValidateGraphicsDescription(desc))));

        if (valid)
        {
            desc.shaderIdentity = shader->GetShader()->GetStableId();
            m_resourceManager.Retain(shader->GetShader());
            RDGPassNode* node = AllocNode<RDGPassNode>(sizeof(RDGPassNode));
            node->neverCull   = !desc.allowCulling;
            node->type        = RDGNodeType::eGraphicsPass;
            node->tag         = desc.passTag;
            node->passDescIdx = static_cast<int32_t>(m_pendingGfxPassDescs.size());
            node->selfStages.SetFlags(RHIPipelineStageFlagBits::eVertexShader,
                                      RHIPipelineStageFlagBits::eFragmentShader);

            const BitField<RHIShaderStageFlagBits> shaderStages =
                shader->GetShader()->GetCreateInfo().stageFlags;

            if (shaderStages.HasFlag(RHIShaderStageFlagBits::eTesselationControl))
            {
                node->selfStages.SetFlag(RHIPipelineStageFlagBits::eTessellationControlShader);
            }

            if (shaderStages.HasFlag(RHIShaderStageFlagBits::eTesselationEvaluation))
            {
                node->selfStages.SetFlag(RHIPipelineStageFlagBits::eTessellationEvaluationShader);
            }

            if (shaderStages.HasFlag(RHIShaderStageFlagBits::eGeometry))
            {
                node->selfStages.SetFlag(RHIPipelineStageFlagBits::eGeometryShader);
            }

            for (uint32_t i = 0; i < desc.colorOutputCount; ++i)
            {
                RDGColorOutputDesc& output = desc.colorOutputs[i];

                if (!output.texture && output.pTexture != nullptr)
                {
                    output.texture = m_resourceManager.ImportTexture(output.pTexture);
                }
                else if (!output.texture)
                {
                    RDGTextureDesc texture{};
                    texture.name      = output.tag;
                    texture.texFormat = {output.format,
                                         TextureDimension::e2D,
                                         output.samples,
                                         output.width,
                                         output.height,
                                         1,
                                         1,
                                         1,
                                         false};
                    texture.usageFlags.SetFlags(RHITextureUsageFlagBits::eColorAttachment,
                                                RHITextureUsageFlagBits::eSampled,
                                                RHITextureUsageFlagBits::eTransferSrc,
                                                RHITextureUsageFlagBits::eTransferDst);
                    output.texture = m_resourceManager.CreateTexture(texture);
                }

                const RDGResourceManager::Allocation* allocation =
                    output.texture ? m_resourceManager.Resolve(output.texture) : nullptr;
                valid =
                    Check(allocation != nullptr, RDGErrorCode::eAttachment, "Invalid color output");

                if (valid)
                {
                    if (!output.tag.IsNone())
                    {
                        m_transientRTMap[output.tag] = {allocation, node};
                    }

                    valid = DeclareTextureAccessForPass(
                        node, allocation, RHITextureUsage::eColorAttachment, FullRange(allocation),
                        RHIAccessMode::eReadWrite, {},
                        AttachmentIntent(output.loadOp, output.fullWrite,
                                         desc.renderArea.minX == 0 && desc.renderArea.minY == 0 &&
                                             desc.renderArea.maxX == int32_t(output.width) &&
                                             desc.renderArea.maxY == int32_t(output.height)),
                        true, output.storeOp == RHIRenderTargetStoreOp::eNone, output.texture);
                }

                if (!valid)
                {
                    break;
                }
            }

            if (valid)
            {
                if (desc.HasDepthStencilOutput())
                {
                    RDGDepthStencilOutputDesc& output = desc.depthStencilOutput;

                    if (!output.texture && output.pTexture != nullptr)
                    {
                        output.texture = m_resourceManager.ImportTexture(output.pTexture);
                    }
                    else if (!output.texture)
                    {
                        RDGTextureDesc texture{};
                        texture.name      = output.tag;
                        texture.texFormat = {output.format,
                                             TextureDimension::e2D,
                                             desc.pipelineStates.multiSampleState.sampleCount,
                                             output.width,
                                             output.height,
                                             1,
                                             1,
                                             1,
                                             false};
                        texture.usageFlags.SetFlags(
                            RHITextureUsageFlagBits::eDepthStencilAttachment,
                            RHITextureUsageFlagBits::eSampled);
                        output.texture = m_resourceManager.CreateTexture(texture);
                    }

                    const RDGResourceManager::Allocation* allocation =
                        output.texture ? m_resourceManager.Resolve(output.texture) : nullptr;
                    valid = Check(allocation != nullptr, RDGErrorCode::eAttachment,
                                  "Invalid depth output");

                    if (valid)
                    {
                        if (!output.tag.IsNone())
                        {
                            m_transientRTMap[output.tag] = {allocation, node};
                        }

                        valid = DeclareTextureAccessForPass(
                            node, allocation, RHITextureUsage::eDepthStencilAttachment,
                            FullRange(allocation), RHIAccessMode::eReadWrite, {},
                            AttachmentIntent(output.loadOp, output.fullWrite,
                                             desc.renderArea.minX == 0 &&
                                                 desc.renderArea.minY == 0 &&
                                                 desc.renderArea.maxX == int32_t(output.width) &&
                                                 desc.renderArea.maxY == int32_t(output.height)),
                            true, output.storeOp == RHIRenderTargetStoreOp::eNone, output.texture);
                    }
                }
            }

            if (valid)
            {
                for (RDGBuffer const buffer : desc.vertexBuffers)
                {
                    valid = DeclareBufferAccessForPass(
                        node, m_resourceManager.Resolve(buffer),
                        BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eVertexBuffer),
                        RHIAccessMode::eRead, {}, RDGContentEffect::eRead, true, false, buffer);

                    if (!valid)
                    {
                        break;
                    }
                }

                valid = valid &&
                    (!(desc.indexBuffer &&
                       !DeclareBufferAccessForPass(
                           node, m_resourceManager.Resolve(desc.indexBuffer),
                           BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eIndexBuffer),
                           RHIAccessMode::eRead, {}, RDGContentEffect::eRead, true, false,
                           desc.indexBuffer)));
            }

            if (valid)
            {
                for (RHIBuffer* buffer : desc.geometryBuffer.vertexBuffers)
                {
                    valid = DeclareBufferAccessForPass(
                        node, m_resourceManager.ImportBufferAllocation(buffer),
                        BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eVertexBuffer),
                        RHIAccessMode::eRead);

                    if (!valid)
                    {
                        break;
                    }
                }
            }

            if (valid)
            {
                if (desc.geometryBuffer.pIndexBuffer != nullptr)
                {
                    valid = DeclareBufferAccessForPass(
                        node,
                        m_resourceManager.ImportBufferAllocation(desc.geometryBuffer.pIndexBuffer),
                        BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eIndexBuffer),
                        RHIAccessMode::eRead);
                }

                valid = valid && (DeclarePassBindingAccess(node, shader, &desc));
            }

            if (valid)
            {
                m_pendingGfxPassDescs.push_back(std::move(desc));

                pRecordedNode = node;
            }
        }
    }

    return {this, pRecordedNode};
}

RDGShaderPassCmdRecorder RenderGraph::AddComputePass(RDGComputePassDesc desc)
{
    RDGPassNode* pRecordedNode = nullptr;

    if (m_result &&
        Check(m_executionState == RDGExecutionState::eBuilding, RDGErrorCode::eLifecycle,
              "AddComputePass requires Begin"))
    {
        ShaderProgram* pShader = ValidatePassDescription(desc, false);

        if (pShader != nullptr)
        {
            desc.shaderIdentity = pShader->GetShader()->GetStableId();
            m_resourceManager.Retain(pShader->GetShader());
            RDGPassNode* pNode = AllocNode<RDGPassNode>(sizeof(RDGPassNode));
            pNode->neverCull   = !desc.allowCulling;
            pNode->type        = RDGNodeType::eComputePass;
            pNode->tag         = desc.passTag.ToString();
            pNode->selfStages.SetFlag(RHIPipelineStageFlagBits::eComputeShader);
            pNode->passDescIdx = static_cast<int32_t>(m_pendingComputePassDescs.size());

            if (DeclarePassBindingAccess(pNode, pShader, &desc))
            {
                m_pendingComputePassDescs.push_back(std::move(desc));
                pRecordedNode = pNode;
            }
        }
    }

    return {this, pRecordedNode};
}

RDGTransferPassCmdRecorder RenderGraph::AddTransferPass(NameID passName)
{
    RDGPassNode* pNode = nullptr;

    if (m_result &&
        Check(m_executionState == RDGExecutionState::eBuilding, RDGErrorCode::eLifecycle,
              "AddTransferPass requires Begin"))
    {
        pNode       = AllocNode<RDGPassNode>(sizeof(RDGPassNode));
        pNode->type = RDGNodeType::eTransferPass;
        pNode->tag  = passName.ToString();
        pNode->selfStages.SetFlag(RHIPipelineStageFlagBits::eTransfer);
        pNode->passDescIdx = static_cast<int32_t>(m_pendingTransferPassDescs.size());
        m_pendingTransferPassDescs.push_back({passName});
        RDGTransferPass* pPass = ZEN_NEW() RDGTransferPass();
        pPass->passTag         = passName;
        m_compiledXferPasses.push_back(pPass);
        pNode->pCompiledPass = pPass;
    }

    return {this, pNode};
}

void RenderGraph::SetPipelineStatesForPassNode(RDGPassNode* node,
                                               BitField<RHIPipelineStageFlagBits> stages)
{
    if (node->type == RDGNodeType::eComputePass)
    {
        stages.ClearFlag(RHIPipelineStageFlagBits::eVertexShader);
        stages.ClearFlag(RHIPipelineStageFlagBits::eFragmentShader);
    }
    else if (node->type == RDGNodeType::eGraphicsPass)
    {
        stages.ClearFlag(RHIPipelineStageFlagBits::eComputeShader);
    }

    node->selfStages.SetFlag(stages);
}

bool RenderGraph::DeclareTextureAccessForPass(const RDGPassNode* node,
                                              const RDGResourceManager::Allocation* resource,
                                              RHITextureUsage usage,
                                              const RHITextureSubResourceRange& range,
                                              RHIAccessMode mode,
                                              BitField<RHIPipelineStageFlagBits> shaderStages,
                                              RDGContentEffect intent,
                                              bool fullCoverage,
                                              bool discardAfter,
                                              RDGResource value)
{
    bool result{};

    if (((ValidateTextureRange(resource, range))) &&
        ((Check(usage > RHITextureUsage::eNone && usage < RHITextureUsage::eMax,
                RDGErrorCode::eBinding, "Invalid texture usage"))))
    {
        const uint32_t required = 1u << (uint32_t(usage) - 1);

        if (Check(!resource->imported || (int64_t(resource->usageFlags) & required) == required,
                  RDGErrorCode::eBinding,
                  "Resource '" + resource->name.ToString() + "' lacks required creation usage"))
        {
            if (!resource->imported)
            {
                const_cast<RDGResourceManager::Allocation*>(resource)->usageFlags.SetFlag(required);
            }

            if (DeclareVersionAccess(node, resource, value, mode))
            {
                RDGAccess access{};
                access.explicitVersion         = value.IsVersioned();
                access.nodeId                  = node->id;
                access.resourceId              = resource->id;
                access.accessMode              = mode;
                access.textureUsage            = usage;
                access.textureSubResourceRange = range;
                access.accessFlags             = RHITextureUsageToAccessFlagBits(usage, mode);
                access.pipelineStages =
                    ResourceStages(node, RHITextureUsageToPipelineStageFlags(usage), shaderStages);

                if (AddResourceAccess(const_cast<RDGPassNode*>(node),
                                      const_cast<RDGResourceManager::Allocation*>(resource),
                                      access))
                {
                    SetPipelineStatesForPassNode(const_cast<RDGPassNode*>(node),
                                                 RHITextureUsageToPipelineStageFlags(usage));

                    if (intent == RDGContentEffect::eAutomatic)
                    {
                        intent = mode == RHIAccessMode::eRead ? RDGContentEffect::eRead :
                                                                RDGContentEffect::eReadWrite;
                    }

                    result = DeclareContentAccess(node, resource, intent, range, fullCoverage,
                                                  discardAfter);
                }
            }
        }
    }

    return result;
}

bool RenderGraph::DeclareBufferAccessForPass(const RDGPassNode* node,
                                             const RDGResourceManager::Allocation* resource,
                                             BitField<RHIBufferUsageFlagBits> usage,
                                             RHIAccessMode mode,
                                             BitField<RHIPipelineStageFlagBits> shaderStages,
                                             RDGContentEffect intent,
                                             bool fullCoverage,
                                             bool discardAfter,
                                             RDGResource value)
{
    bool result{};

    if (Check(resource != nullptr && resource->type == RDGResourceType::eBuffer,
              RDGErrorCode::eBinding, "Invalid buffer access"))
    {
        const uint32_t required = uint32_t(int64_t(usage));

        if (Check(required != 0 && (required & ~0x1ffu) == 0, RDGErrorCode::eBinding,
                  "Invalid buffer usage"))
        {
            if (Check(!resource->imported || (int64_t(resource->usageFlags) & required) == required,
                      RDGErrorCode::eBinding,
                      "Resource '" + resource->name.ToString() + "' lacks required creation usage"))
            {
                if (!resource->imported)
                {
                    const_cast<RDGResourceManager::Allocation*>(resource)->usageFlags.SetFlag(
                        required);
                }

                if (DeclareVersionAccess(node, resource, value, mode))
                {
                    RDGAccess access{};
                    access.explicitVersion = value.IsVersioned();
                    access.nodeId          = node->id;
                    access.resourceId      = resource->id;
                    access.accessMode      = mode;
                    access.bufferUsage     = usage;
                    access.pipelineStages  = ResourceStages(
                        node, RHIBufferUsageToPipelineStageFlags(usage), shaderStages);

                    for (uint32_t i = 0; i < 9; ++i)
                    {
                        if (int64_t(usage) & (1u << i))
                        {
                            access.accessFlags.SetFlag(RHIBufferUsageToAccessFlagBits(
                                static_cast<RHIBufferUsage>(i + 1), mode));
                        }
                    }

                    if (AddResourceAccess(const_cast<RDGPassNode*>(node),
                                          const_cast<RDGResourceManager::Allocation*>(resource),
                                          access))
                    {
                        SetPipelineStatesForPassNode(const_cast<RDGPassNode*>(node),
                                                     RHIBufferUsageToPipelineStageFlags(usage));

                        if (intent == RDGContentEffect::eAutomatic)
                        {
                            intent = mode == RHIAccessMode::eRead ? RDGContentEffect::eRead :
                                                                    RDGContentEffect::eReadWrite;
                        }

                        result = DeclareContentAccess(node, resource, intent, {}, fullCoverage,
                                                      discardAfter);
                    }
                }
            }
        }
    }

    return result;
}

bool RenderGraph::DeclareContentAccess(const RDGPassNode* node,
                                       const RDGResourceManager::Allocation* resource,
                                       RDGContentEffect intent,
                                       const RHITextureSubResourceRange& range,
                                       bool fullCoverage,
                                       bool discardAfter)
{
    bool result{};

    if (Check(resource != nullptr && intent >= RDGContentEffect::eRead &&
                  intent <= RDGContentEffect::eFullWrite &&
                  (!IsProducedElementIntent(intent) || resource->type == RDGResourceType::eBuffer),
              RDGErrorCode::eBinding, "Invalid content access intent"))
    {
        const_cast<RDGPassNode*>(node)->contentAccesses.push_back(
            {resource->id, intent, range, fullCoverage, discardAfter});
        result = true;
    }

    return result;
}

bool RenderGraph::ApplyContentStatus(const RDGPassNode* node,
                                     const RDGContentAccess& access,
                                     bool read,
                                     bool write,
                                     RDGContentStatus produced,
                                     std::unordered_set<int32_t>& warned,
                                     RDGContentStatus& status)
{
    RDGResourceContent& contents = m_finalContents[access.resourceId];
    const RDGResourceManager::Allocation* resource =
        m_resourceManager.FindResourceByIdx(access.resourceId);

    bool valid = true;

    if (read &&
        (access.intent == RDGContentEffect::eRead ||
         access.intent == RDGContentEffect::eReadWrite || access.requiresPriorContents))
    {
        if (status == RDGContentStatus::eUndefined)
        {
            valid = Fail(RDGErrorCode::eUninitialized,
                         "Pass '" + node->tag.ToString() + "' reads undefined contents of '" +
                             resource->name.ToString() + "'");
        }

        if ((valid) && (status == RDGContentStatus::eUnknown))
        {
            if (node->requireDefinedContents)
            {
                valid = Fail(RDGErrorCode::eExport,
                             "Extraction requires defined contents of '" +
                                 resource->name.ToString() + "'");
            }

            if ((valid) && (warned.insert(int32_t(resource->id)).second))
            {
                const RHIResource* physical = PhysicalResource(resource);
                const std::string name      = !resource->name.IsNone() ?
                    resource->name.ToString() :
                    std::string(resource->type == RDGResourceType::eBuffer ? "buffer #" :
                                                                             "texture #") +
                        std::to_string(physical ? physical->GetStableId() :
                                                  uint64_t(int32_t(resource->id)));
                const std::string message   = "Pass '" + node->tag.ToString() +
                    "': initialization coverage is unknown for '" + name + "'";
                m_warnings.push_back({RDGErrorCode::eUnknownContents, message});

                if (!contents.unknownWarningLogged)
                {
                    m_pendingContentWarnings.push_back(m_warnings.back());
                    contents.unknownWarningLogged = true;
                }
            }
        }
    }

    if ((valid) && (write))
    {
        if (access.intent == RDGContentEffect::eDiscardWrite ||
            access.intent == RDGContentEffect::eFullWrite)
        {
            status = RDGContentStatus::eUndefined;
        }

        if (access.intent == RDGContentEffect::eFullWrite && access.fullCoverage)
        {
            status = produced;
        }

        if (access.intent == RDGContentEffect::eWrite && access.sourceResourceId.IsValid() &&
            produced != RDGContentStatus::eDefined && status == RDGContentStatus::eDefined)
        {
            status = produced;
        }

        if (access.discardAfter)
        {
            status = RDGContentStatus::eUndefined;
        }
    }

    return valid;
}

bool RenderGraph::ApplyContentAccess(std::unordered_set<int32_t>& warned,
                                     const RDGPassNode* node,
                                     const RDGContentAccess& access,
                                     bool read,
                                     bool write)
{
    RDGResourceContent& contents = m_finalContents[access.resourceId];
    const RDGResourceManager::Allocation* resource =
        m_resourceManager.FindResourceByIdx(access.resourceId);
    RDGContentStatus produced = RDGContentStatus::eDefined;

    if (access.sourceResourceId.IsValid())
    {
        RDGResourceContent const& source = m_finalContents[access.sourceResourceId];
        const RDGResourceManager::Allocation* sourceResource =
            m_resourceManager.FindResourceByIdx(access.sourceResourceId);
        produced = sourceResource->type == RDGResourceType::eBuffer ?
            BufferContents(source, access.sourceBufferOffset,
                           access.sourceBufferSize ? access.sourceBufferSize :
                                                     sourceResource->bufferSize) :
            source.status;

        if (sourceResource->type == RDGResourceType::eTexture)
        {
            produced = RDGContentStatus::eDefined;

            for (uint32_t aspect = 0; aspect < 3; ++aspect)
            {
                if (int64_t(access.sourceRange.aspect) & (1u << aspect))
                {
                    for (uint32_t mip = access.sourceRange.baseMipLevel;
                         mip < access.sourceRange.baseMipLevel + access.sourceRange.levelCount;
                         ++mip)
                    {
                        for (uint32_t layer = access.sourceRange.baseArrayLayer; layer <
                             access.sourceRange.baseArrayLayer + access.sourceRange.layerCount;
                             ++layer)
                        {
                            const uint64_t key =
                                (uint64_t(aspect) << 62) | (uint64_t(mip) << 32) | layer;
                            const std::unordered_map<uint64_t, RDGContentStatus>::const_iterator
                                found = source.textureSubresources.find(key);
                            const RDGContentStatus status =
                                found == source.textureSubresources.end() ? source.status :
                                                                            found->second;

                            if (status != RDGContentStatus::eDefined)
                            {
                                produced = status;
                            }
                        }
                    }
                }
            }
        }
    }

    bool applied = true;

    if (resource->type == RDGResourceType::eBuffer)
    {
        const uint64_t offset   = access.bufferOffset;
        const uint64_t size     = access.bufferSize ? access.bufferSize : resource->bufferSize;
        RDGContentStatus status = BufferContents(contents, offset, size);

        if (read && access.intent == RDGContentEffect::eReadProducedElements &&
            !contents.hasProducedElements && status != RDGContentStatus::eDefined)
        {
            applied = Fail(RDGErrorCode::eUninitialized,
                           "Pass '" + node->tag.ToString() +
                               "' requires a declared element producer for '" +
                               resource->name.ToString() + "'");
        }

        applied =
            applied && ApplyContentStatus(node, access, read, write, produced, warned, status);

        if (applied && write)
        {
            if (access.intent == RDGContentEffect::eFullWrite ||
                access.intent == RDGContentEffect::eDiscardWrite || access.discardAfter)
            {
                SetBufferContents(contents, offset, size, resource->bufferSize, status);
                contents.hasProducedElements = false;
            }
            else if (access.intent == RDGContentEffect::eWrite && access.sourceResourceId.IsValid())
            {
                // A copy defines exactly its destination bytes, even when it is
                // one of several chunks submitted by separate upload graphs.
                SetBufferContents(contents, offset, size, resource->bufferSize, produced);

                if (produced != RDGContentStatus::eDefined)
                {
                    contents.hasProducedElements = false;
                }
            }
            else if (access.intent == RDGContentEffect::eWriteProducedElements)
            {
                contents.hasProducedElements = true;
            }
        }
    }
    else
    {
        for (uint32_t aspect = 0; applied && aspect < 3; ++aspect)
        {
            if ((int64_t(access.range.aspect) & (1u << aspect)) == 0)
            {
                continue;
            }

            for (uint32_t mip = access.range.baseMipLevel;
                 applied && mip < access.range.baseMipLevel + access.range.levelCount; ++mip)
            {
                for (uint32_t layer = access.range.baseArrayLayer;
                     applied && layer < access.range.baseArrayLayer + access.range.layerCount;
                     ++layer)
                {
                    const uint64_t key = (uint64_t(aspect) << 62) | (uint64_t(mip) << 32) | layer;
                    const std::pair<HashMap<uint64_t, RDGContentStatus>::iterator, bool> itResult =
                        contents.textureSubresources.try_emplace(key, contents.status);
                    HashMap<uint64_t, RDGContentStatus>::iterator it = itResult.first;
                    applied =
                        ApplyContentStatus(node, access, read, write, produced, warned, it->second);
                }
            }
        }
    }

    return applied;
}

bool RenderGraph::ValidateContents(const ResourceStateTracker& tracker)
{
    bool valid = true;

    m_warnings.clear();
    m_pendingContentWarnings.clear();
    m_finalContents.clear();

    for (const RDGResourceManager::Allocation* resource : m_resourceManager.m_resources)
    {
        RDGResourceContent contents;

        if (!resource->imported)
        {
            contents.status = RDGContentStatus::eUndefined;
        }
        else if (resource->initialContents == RDGImportContents::ePreserve)
        {
            contents = tracker.GetContents(PhysicalResource(resource));
        }
        else
        {
            contents.status = resource->initialContents == RDGImportContents::eDefined ?
                RDGContentStatus::eDefined :
                resource->initialContents == RDGImportContents::eUndefined ?
                RDGContentStatus::eUndefined :
                RDGContentStatus::eUnknown;
            contents.unknownWarningLogged =
                tracker.GetContents(PhysicalResource(resource)).unknownWarningLogged;
        }

        m_finalContents.push_back(std::move(contents));
    }

    std::unordered_set<int32_t> warned;

    for (RDG_ID const id : m_sortedNodes)
    {
        const RDGPassNode* node = static_cast<const RDGPassNode*>(GetNodeBaseById(id));

        if (node->type == RDGNodeType::eTransferPass)
        {
            for (RDGContentAccess const& access : node->contentAccesses)
            {
                valid = ApplyContentAccess(warned, node, access, true, true);

                if (!valid)
                {
                    break;
                }
            }
        }
        else
        {
            // Shader bindings are simultaneous declarations, never an initialization sequence.
            for (RDGContentAccess const& access : node->contentAccesses)
            {
                valid = ApplyContentAccess(warned, node, access, true, false);

                if (!valid)
                {
                    break;
                }
            }

            if (valid)
            {
                for (RDGContentAccess const& access : node->contentAccesses)
                {
                    valid = ApplyContentAccess(warned, node, access, false, true);

                    if (!valid)
                    {
                        break;
                    }
                }
            }
        }

        if (!valid)
        {
            break;
        }
    }

    return valid;
}

void RenderGraph::CommitContents(ResourceStateTracker& tracker)
{
    // Several logical allocations may share storage. Commit the last scheduled occupant,
    // independently of resource creation order. Logical validation remains per allocation.
    for (uint32_t order = 0; order < m_sortedNodes.size(); ++order)
    {
        const RDGNodeBase* node = GetNodeBaseById(m_sortedNodes[order]);

        for (uint32_t i = 0; i < node->accessCount; ++i)
        {
            RDGAccess const& access = m_accesses[node->accessOffset + i];
            const RDGResourceManager::Allocation* resource =
                m_resourceManager.FindResourceByIdx(access.resourceId);

            if (resource->lastUse == order)
            {
                tracker.SetContents(PhysicalResource(resource), m_finalContents[resource->id]);
            }
        }
    }
}

bool RenderGraph::DeclareTextureBindings(RDGPassNode* node,
                                         const RDGPassDescBase* desc,
                                         const ShaderProgram* shader,
                                         const HeapVector<RDGTextureBinding>& bindings,
                                         RHITextureUsage usage)
{
    bool valid = true;

    for (const RDGTextureBinding& binding : bindings)
    {
        const RHIShaderResourceDescriptor* descriptor =
            shader->GetShaderResourceDescriptor(binding.glslName);
        valid = Check(descriptor != nullptr, RDGErrorCode::eBinding, "Unknown texture binding");
        valid = valid &&
            (Check(uint64_t(binding.views.offset) + binding.views.count <=
                       desc->textureViewStorage.size(),
                   RDGErrorCode::eBinding, "Invalid texture binding slice"));

        if (valid)
        {
            for (uint32_t i = 0; i < binding.views.count; ++i)
            {
                RHITextureView* view = desc->textureViewStorage[binding.views.offset + i];
                valid = Check(view != nullptr, RDGErrorCode::eBinding, "Null texture view");

                if (valid)
                {
                    const RDGResourceManager::Allocation* resource =
                        m_resourceManager.ImportTextureAllocation(view->GetTexture());
                    m_resourceManager.Retain(view);
                    m_resourceManager.Retain(binding.pSampler);
                    valid = DeclareTextureAccessForPass(
                        node, resource, usage, view->GetSubResourceRange(),
                        BindingMode(*descriptor), ShaderPipelineStages(descriptor->stageFlags),
                        BindingEffect(*descriptor, binding.contents));

                    if (valid)
                    {
                        node->contentAccesses.back().requiresPriorContents =
                            descriptor->readable && descriptor->writable;
                    }
                }

                if (!valid)
                {
                    break;
                }
            }
        }

        if (!valid)
        {
            break;
        }
    }

    return valid;
}

bool RenderGraph::DeclarePassBindingAccess(RDGPassNode* node,
                                           ShaderProgram* shader,
                                           RDGPassDescBase* desc)
{
    bool valid = true;

    for (RDGBuffer const buffer : desc->logicalIndirectBuffers)
    {
        valid = DeclareBufferAccessForPass(
            node, m_resourceManager.Resolve(buffer),
            BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eIndirectBuffer),
            RHIAccessMode::eRead, {}, RDGContentEffect::eRead, true, false, buffer);

        if (!valid)
        {
            break;
        }
    }

    if (valid)
    {
        for (RHIBuffer* buffer : desc->indirectBuffers)
        {
            valid = DeclareBufferAccessForPass(
                node, m_resourceManager.ImportBufferAllocation(buffer),
                BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eIndirectBuffer),
                RHIAccessMode::eRead);

            if (!valid)
            {
                break;
            }
        }
    }

    if (valid)
    {
        for (RDGBufferBinding const& binding : desc->UAVBufferBindings)
        {
            const RHIShaderResourceDescriptor* descriptor =
                shader->GetShaderResourceDescriptor(binding.glslName);
            valid = Check(descriptor != nullptr, RDGErrorCode::eBinding,
                          "Unknown storage buffer binding");
            valid = valid &&
                (Check(uint64_t(binding.buffers.offset) + binding.buffers.count <=
                           desc->bufferStorage.size(),
                       RDGErrorCode::eBinding, "Invalid buffer binding slice"));

            if (valid)
            {
                for (uint32_t i = 0; i < binding.buffers.count; ++i)
                {
                    const RDGResourceManager::Allocation* resource =
                        m_resourceManager.ImportBufferAllocation(
                            desc->bufferStorage[binding.buffers.offset + i]);
                    valid = DeclareBufferAccessForPass(
                        node, resource,
                        BitField<RHIBufferUsageFlagBits>(RHIBufferUsageFlagBits::eStorageBuffer),
                        BindingMode(*descriptor), ShaderPipelineStages(descriptor->stageFlags),
                        BindingEffect(*descriptor, binding.contents));

                    if (valid)
                    {
                        node->contentAccesses.back().requiresPriorContents =
                            descriptor->readable && descriptor->writable;
                    }

                    if (!valid)
                    {
                        break;
                    }
                }
            }

            if (!valid)
            {
                break;
            }
        }
    }

    if (valid)
    {
        valid = DeclareTextureBindings(node, desc, shader, desc->sampledTexBindings,
                                       RHITextureUsage::eSampled);

        valid = valid &&
            DeclareTextureBindings(node, desc, shader, desc->separateTexBindings,
                                   RHITextureUsage::eSampled);

        valid = valid &&
            (DeclareTextureBindings(node, desc, shader, desc->UAVTexBindings,
                                    RHITextureUsage::eStorage));

        if (valid)
        {
            for (RDGResourceBinding const& binding : desc->resourceBindings)
            {
                const RHIShaderResourceDescriptor* descriptor =
                    shader->GetShaderResourceDescriptor(binding.glslName);
                valid =
                    Check(descriptor != nullptr, RDGErrorCode::eBinding, "Unknown logical binding");

                if (valid)
                {
                    const RHIShaderResourceType type = LogicalBindingType(binding.resourceType);
                    const RDGContentEffect effect    = BindingEffect(*descriptor, binding.contents);
                    const BitField<RHIPipelineStageFlagBits> stages =
                        ShaderPipelineStages(descriptor->stageFlags);

                    for (uint32_t i = 0; i < binding.resources.count; ++i)
                    {
                        RDGBoundResource& element =
                            desc->resourceStorage[binding.resources.offset + i];

                        if (!element.resource)
                        {
                            const std::unordered_map<
                                NameID, RenderGraph::RDGTransientOutput>::iterator found =
                                m_transientRTMap.find(binding.producerOutputTag);
                            valid = Check(found != m_transientRTMap.end(), RDGErrorCode::eBinding,
                                          "Logical input has no preceding producer");

                            if (valid)
                            {
                                element.resource =
                                    m_resourceManager.MakeResource(found->second.pResource);
                            }
                        }

                        if (valid)
                        {
                            const RDGResourceManager::Allocation* resource =
                                m_resourceManager.Resolve(element.resource);
                            valid = (resource != nullptr);

                            if (valid)
                            {
                                if (resource->type == RDGResourceType::eBuffer)
                                {
                                    const RHIBufferUsageFlagBits usage =
                                        type == RHIShaderResourceType::eUniformBuffer ?
                                        RHIBufferUsageFlagBits::eUniformBuffer :
                                        RHIBufferUsageFlagBits::eStorageBuffer;
                                    valid = DeclareBufferAccessForPass(
                                        node, resource, BitField<RHIBufferUsageFlagBits>(usage),
                                        BindingMode(*descriptor), stages, effect, true, false,
                                        element.resource);
                                }
                                else
                                {
                                    valid = (m_resourceManager.ResolveView(
                                                 element.resource, element.view) != nullptr);

                                    if (valid)
                                    {
                                        m_resourceManager.Retain(binding.pSampler);
                                        valid = DeclareTextureAccessForPass(
                                            node, resource,
                                            type == RHIShaderResourceType::eImage ?
                                                RHITextureUsage::eStorage :
                                                RHITextureUsage::eSampled,
                                            m_resourceManager.ViewRange(*resource, element.view),
                                            BindingMode(*descriptor), stages, effect, true, false,
                                            element.resource);
                                    }
                                }
                            }

                            if (valid)
                            {
                                node->contentAccesses.back().requiresPriorContents =
                                    descriptor->readable && descriptor->writable;
                            }
                        }

                        if (!valid)
                        {
                            break;
                        }
                    }
                }

                if (!valid)
                {
                    break;
                }
            }
        }
    }

    return valid;
}

bool RenderGraph::AddResourceAccess(RDGPassNode* node,
                                    RDGResourceManager::Allocation* resource,
                                    const RDGAccess& access)
{
    bool valid = Check(m_executionState == RDGExecutionState::eBuilding, RDGErrorCode::eLifecycle,
                       "Resource declarations require a building graph");

    if (valid)
    {
        RDGAccess* pExisting = static_cast<RDGAccess*>(nullptr);

        for (RDGAccess& existing : node->pendingAccesses)
        {
            if (existing.resourceId == access.resourceId)
            {
                pExisting = &existing;
                break;
            }
        }

        if (pExisting != nullptr)
        {
            RDGAccess& existing = *pExisting;

            if (existing.explicitVersion != access.explicitVersion)
            {
                valid = Fail(RDGErrorCode::eVersion,
                             "Pass '" + node->tag.ToString() +
                                 "' mixes automatic and explicit version declarations for '" +
                                 resource->name.ToString() + "'");
            }

            valid = valid &&
                Check(resource->type != RDGResourceType::eTexture ||
                          RHITextureUsageToLayout(existing.textureUsage) ==
                              RHITextureUsageToLayout(access.textureUsage),
                      RDGErrorCode::eConflictingLayout,
                      "Pass '" + node->tag.ToString() + "': conflicting texture layouts for '" +
                          resource->name.ToString() +
                          "'; split the operations into separate passes");

            if (valid)
            {
                if (access.accessMode == RHIAccessMode::eReadWrite)
                {
                    existing.accessMode = access.accessMode;
                }

                existing.bufferUsage.SetFlag(access.bufferUsage);
                existing.accessFlags.SetFlag(access.accessFlags);
                existing.pipelineStages.SetFlag(access.pipelineStages);

                if (resource->type == RDGResourceType::eTexture)
                {
                    existing.textureSubResourceRange = FullRange(resource);
                }
            }
        }
        else
        {
            node->pendingAccesses.push_back(access);
            ++node->accessCount;
            ++resource->accessCount;
        }
    }

    return valid;
}

bool RenderGraph::DeclareVersionAccess(const RDGPassNode* node,
                                       const RDGResourceManager::Allocation* resource,
                                       RDGResource value,
                                       RHIAccessMode mode)
{
    bool valid = true;

    if (value.IsVersioned())
    {
        const RDGResourceManager::Allocation* resolved = m_resourceManager.Resolve(value);
        valid                                          = resolved != nullptr &&
            Check(resolved == resource, RDGErrorCode::eVersion,
                  "Resource version/allocation mismatch");

        if (valid)
        {
            const RDGResourceManager::ResourceVersion& version =
                m_resourceManager.m_versions[value.m_version];
            const bool writes = mode != RHIAccessMode::eRead;
            valid =
                Check(!writes || version.previous >= 0, RDGErrorCode::eVersion,
                      "Pass '" + node->tag.ToString() + "' cannot write initial version 0 of '" +
                          resource->name.ToString() + "'");

            if (valid)
            {
                HeapVector<RDGVersionAccess>& accesses =
                    const_cast<RDGPassNode*>(node)->versionAccesses;
                bool found = false;

                for (RDGVersionAccess const& prior : accesses)
                {
                    if (prior.resourceId != resource->id)
                    {
                        continue;
                    }

                    valid =
                        Check(node->type != RDGNodeType::eTransferPass || prior.writes == writes,
                              RDGErrorCode::eVersion,
                              "Transfer pass '" + node->tag.ToString() +
                                  "' reads and writes versions of the same allocation '" +
                                  resource->name.ToString() +
                                  "'; split the passes to preserve the selected contents") &&
                        Check(!writes || !prior.writes || prior.version == value.m_version,
                              RDGErrorCode::eVersion,
                              "Pass '" + node->tag.ToString() + "' writes multiple versions of '" +
                                  resource->name.ToString() + "'; split the passes");
                    found = prior.version == value.m_version && prior.writes == writes;

                    if (!valid || found)
                    {
                        break;
                    }
                }

                if (valid && !found)
                {
                    accesses.push_back({resource->id, value.m_version, writes});
                }
            }
        }
    }

    return valid;
}

void RenderGraph::AddDependency(HeapVector<HeapVector<uint32_t>>& adjacency,
                                HashMap<uint64_t, bool>& edges,
                                RDGDependency dependency)
{
    if (dependency.source == dependency.destination || !dependency.source.IsValid())
    {
        return;
    }

    const RDGResourceManager::Allocation* resource =
        m_resourceManager.FindResourceByIdx(dependency.resourceId);

    if (resource->type == RDGResourceType::eTexture)
    {
        dependency.textureRange = FullRange(resource);
    }
    else
    {
        dependency.bufferSize = resource->bufferSize;
    }

    const uint32_t index = uint32_t(m_dependencies.size());
    m_dependencies.push_back(dependency);

    if (edges.emplace(CreateNodePairKey(dependency.source, dependency.destination), true).second)
    {
        adjacency[dependency.source].push_back(index);
        ++m_inDegrees[dependency.destination];
    }
}

bool RenderGraph::BuildVersionDependencies(HeapVector<HeapVector<uint32_t>>& adjacency,
                                           HashMap<uint64_t, bool>& edges)
{
    bool valid = true;

    struct VersionUsers
    {
        RDG_ID producer{-1};
        HeapVector<RDG_ID> readers;
    };

    const HeapVector<RDGResourceManager::ResourceVersion>& versions = m_resourceManager.m_versions;
    HeapVector<VersionUsers> users(versions.size());

    for (const RDGNodeBase* base : m_nodes)
    {
        const RDGPassNode* node = static_cast<const RDGPassNode*>(base);

        for (RDGVersionAccess const& access : node->versionAccesses)
        {
            VersionUsers& use = users[access.version];

            if (access.writes)
            {
                const RDGResourceManager::ResourceVersion& version = versions[access.version];

                if (use.producer.IsValid() && use.producer != node->id)
                {
                    valid = Fail(RDGErrorCode::eDuplicateProducer,
                                 "Version " + std::to_string(version.number) + " of '" +
                                     m_resourceManager.FindResourceByIdx(access.resourceId)
                                         ->name.ToString() +
                                     "' has two producers: '" +
                                     GetNodeBaseById(use.producer)->tag.ToString() + "' and '" +
                                     node->tag.ToString() + "'");
                }

                if (valid)
                {
                    use.producer = node->id;
                }
            }
            else
            {
                use.readers.push_back(node->id);
            }

            if (!valid)
            {
                break;
            }
        }

        if (!valid)
        {
            break;
        }
    }

    if (valid)
    {
        for (size_t i = 0; i < versions.size(); ++i)
        {
            const RDGResourceManager::ResourceVersion& version = versions[i];
            VersionUsers const& use                            = users[i];
            const RDGResourceManager::Allocation* resource =
                m_resourceManager.FindResourceByIdx(version.resourceId);

            if (version.previous >= 0 && !use.producer.IsValid())
            {
                valid = Fail(RDGErrorCode::eMissingProducer,
                             "Version " + std::to_string(version.number) + " of '" +
                                 resource->name.ToString() + "' has no producer" +
                                 (use.readers.empty() ? std::string{} :
                                                        "; consumed by '" +
                                          GetNodeBaseById(use.readers[0])->tag.ToString() + "'"));
            }

            if (valid)
            {
                for (RDG_ID reader : use.readers)
                {
                    valid = Check(
                        reader != use.producer, RDGErrorCode::eVersion,
                        "Pass '" + GetNodeBaseById(reader)->tag.ToString() +
                            "' reads its own output version " + std::to_string(version.number) +
                            " of '" + resource->name.ToString() +
                            "'; use ReadWrite on the output to read its predecessor, or split the passes");

                    if (valid)
                    {
                        AddDependency(adjacency, edges,
                                      {use.producer, reader, version.resourceId,
                                       int32_t(version.number),
                                       RDGDependencyReason::eVersionProducer});
                    }

                    if (!valid)
                    {
                        break;
                    }
                }
            }

            if (valid)
            {
                if (version.previous < 0)
                {
                    continue;
                }

                VersionUsers const& previous = users[version.previous];
                AddDependency(adjacency, edges,
                              {previous.producer, use.producer, version.resourceId,
                               int32_t(version.number), RDGDependencyReason::eWriteAfterWrite});

                for (RDG_ID reader : previous.readers)
                {
                    AddDependency(adjacency, edges,
                                  {reader, use.producer, version.resourceId,
                                   int32_t(version.number - 1),
                                   RDGDependencyReason::eWriteAfterRead});
                }
            }

            if (!valid)
            {
                break;
            }
        }
    }

    return valid;
}

bool RenderGraph::ReportDependencyCycle(const HeapVector<HeapVector<uint32_t>>& adjacency)
{
    bool valid = true;

    struct SearchFrame
    {
        RDG_ID node;
        size_t next{0};
    };

    HeapVector<uint8_t> colors(m_nodeCount);
    HeapVector<uint32_t> parents(m_nodeCount);
    HeapVector<SearchFrame> stack;

    for (uint32_t root = 0; root < m_nodeCount; ++root)
    {
        if (colors[root] != 0)
        {
            continue;
        }

        colors[root] = 1;
        stack.push_back({RDG_ID(root), 0});

        while (!stack.empty())
        {
            SearchFrame& frame = stack.back();

            if (frame.next == adjacency[frame.node].size())
            {
                colors[frame.node] = 2;
                stack.pop_back();
                continue;
            }

            uint32_t const edgeIndex  = adjacency[frame.node][frame.next++];
            RDGDependency const& edge = m_dependencies[edgeIndex];

            if (colors[edge.destination] == 1)
            {
                HeapVector<uint32_t> cycle;
                cycle.push_back(edgeIndex);

                for (RDG_ID node = edge.source; node != edge.destination;)
                {
                    uint32_t const parent = parents[node];
                    cycle.push_back(parent);
                    node = m_dependencies[parent].source;
                }

                std::string message = "RenderGraph dependency cycle:";

                for (size_t i = cycle.size(); i-- > 0;)
                {
                    RDGDependency const& cause = m_dependencies[cycle[i]];
                    const char* reason         = "producer";

                    switch (cause.reason)
                    {
                        case RDGDependencyReason::eWriteAfterRead: reason = "WAR"; break;
                        case RDGDependencyReason::eWriteAfterWrite: reason = "WAW"; break;
                        case RDGDependencyReason::eVersionProducer: break;
                    }

                    message += " [#" + std::to_string(int32_t(cause.source)) + " '" +
                        GetNodeBaseById(cause.source)->tag.ToString() + "' -> #" +
                        std::to_string(int32_t(cause.destination)) + " '" +
                        GetNodeBaseById(cause.destination)->tag.ToString() + "': " + reason + " '" +
                        m_resourceManager.FindResourceByIdx(cause.resourceId)->name.ToString() +
                        "'";

                    if (cause.version >= 0)
                    {
                        message += " v" + std::to_string(cause.version);
                    }

                    message += "]";
                }

                valid = Fail(RDGErrorCode::eDependencyCycle, message);
            }

            if ((valid) && (colors[edge.destination] == 0))
            {
                colors[edge.destination]  = 1;
                parents[edge.destination] = edgeIndex;
                stack.push_back({edge.destination, 0});
            }

            if (!valid)
            {
                break;
            }
        }

        if (!valid)
        {
            break;
        }
    }

    if (valid)
    {
        valid = Fail(RDGErrorCode::eDependencyCycle,
                     "RenderGraph has an incomplete dependency schedule");
    }

    return valid;
}

bool RenderGraph::FinalizeResourceVersions()
{
    bool valid = true;

    // Select values while finalizing declarations, before scheduling. Automatic bindings
    // denote the value at this point in the builder's pass sequence. Explicit handles already
    // name their value and never participate in that selection. Both produce the same IR.
    for (const RDGNodeBase* base : m_nodes)
    {
        const RDGPassNode* node = static_cast<const RDGPassNode*>(base);

        for (RDGAccess const& access : node->pendingAccesses)
        {
            const RDGResourceManager::Allocation* resource =
                m_resourceManager.FindResourceByIdx(access.resourceId);

            if (!access.explicitVersion && resource->initialVersion >= 0)
            {
                valid = Fail(RDGErrorCode::eVersion,
                             "Pass '" + node->tag.ToString() +
                                 "' uses an automatic binding for explicitly versioned resource '" +
                                 resource->name.ToString() + "'");
            }

            if (!valid)
            {
                break;
            }
        }

        if (!valid)
        {
            break;
        }
    }

    if (valid)
    {
        HeapVector<RDGResource> current(m_resourceManager.m_resources.size());

        for (RDGNodeBase* base : m_nodes)
        {
            RDGPassNode* node = static_cast<RDGPassNode*>(base);

            for (RDGAccess const& access : node->pendingAccesses)
            {
                if (access.explicitVersion)
                {
                    continue;
                }

                RDGResource& value = current[access.resourceId];

                if (!value)
                {
                    value = m_resourceManager.InitialResourceVersion(m_resourceManager.MakeResource(
                        m_resourceManager.FindResourceByIdx(access.resourceId)));
                }

                const bool writes = access.accessMode != RHIAccessMode::eRead;

                if (value && writes)
                {
                    value = m_resourceManager.CreateResourceVersion(value);
                }

                valid = static_cast<bool>(value);

                // A merged writing access consumes/preserves the predecessor as required by
                // its content intents; in-pass transfer commands retain their recorded order.
                if (valid)
                {
                    node->versionAccesses.push_back({access.resourceId, value.m_version, writes});
                }

                if (!valid)
                {
                    break;
                }
            }

            if (!valid)
            {
                break;
            }
        }
    }

    return valid;
}

bool RenderGraph::SortNodesByVersion()
{
    bool result{};

    m_sortedNodes.clear();
    m_dependencies.clear();
    m_inDegrees.clear();
    m_inDegrees.resize(m_nodeCount);
    HeapVector<HeapVector<uint32_t>> adjacency(m_nodeCount);
    HashMap<uint64_t, bool> edges;

    if (BuildVersionDependencies(adjacency, edges))
    {
        // Readiness comes exclusively from version dependencies. IDs are lookup keys, never
        // priorities. Newly ready passes join the work queue without preempting other ready work.
        // Independent passes have no required relative order; layouts are handled by barriers
        // generated after this schedule, not by artificial reader-to-reader dependencies.
        std::queue<const RDGNodeBase*> ready;

        for (const RDGNodeBase* node : m_nodes)
        {
            if (m_inDegrees[node->id] == 0)
            {
                ready.push(node);
            }
        }

        while (!ready.empty())
        {
            const RDGNodeBase* node = ready.front();
            ready.pop();
            m_sortedNodes.push_back(node->id);

            for (uint32_t const edgeIndex : adjacency[node->id])
            {
                const RDG_ID next = m_dependencies[edgeIndex].destination;

                if (--m_inDegrees[next] == 0)
                {
                    ready.push(GetNodeBaseById(next));
                }
            }
        }

        if (m_sortedNodes.size() != m_nodeCount)
        {
            result = ReportDependencyCycle(adjacency);
        }
        else
        {
            m_compileStats.dependencyEdgeCount = static_cast<uint32_t>(edges.size());
            result                             = true;
        }
    }

    return result;
}

bool RenderGraph::SetOptimizations(bool cullPasses, bool reuseAllocations)
{
    bool result{};

    if (Check(m_executionState == RDGExecutionState::eIdle, RDGErrorCode::eLifecycle,
              "Set RDG optimizations before Begin"))
    {
        m_cullPasses       = cullPasses;
        m_reuseAllocations = reuseAllocations;
        result             = true;
    }

    return result;
}

void RenderGraph::DetermineLiveness()
{
    HeapVector<HeapVector<RDG_ID>> predecessors(m_nodeCount);

    for (RDGDependency const& dependency : m_dependencies)
    {
        // WAR orders storage reuse; it does not make an otherwise dead reader observable.
        // Keep prior writers conservatively: partial writes may preserve predecessor contents.
        if (dependency.reason != RDGDependencyReason::eWriteAfterRead)
        {
            predecessors[dependency.destination].push_back(dependency.source);
        }
    }

    HeapVector<RDG_ID> work;

    for (RDGNodeBase* base : m_nodes)
    {
        RDGPassNode* node = static_cast<RDGPassNode*>(base);
        node->live        = !m_cullPasses || node->neverCull || node->requireDefinedContents;

        for (RDGVersionAccess const& access : node->versionAccesses)
        {
            node->live |=
                access.writes && m_resourceManager.FindResourceByIdx(access.resourceId)->imported;
        }

        if (node->live)
        {
            work.push_back(node->id);
        }
    }

    while (!work.empty())
    {
        RDG_ID const id = work.back();
        work.pop_back();

        for (RDG_ID const previous : predecessors[id])
        {
            RDGPassNode* node = static_cast<RDGPassNode*>(GetNodeBaseById(previous));

            if (!node->live)
            {
                node->live = true;
                work.push_back(previous);
            }
        }
    }

    m_sortedNodes.erase(
        std::remove_if(m_sortedNodes.begin(), m_sortedNodes.end(),
                       [this](RDG_ID id) {
                           return !static_cast<const RDGPassNode*>(GetNodeBaseById(id))->live;
                       }),
        m_sortedNodes.end());
    m_dependencies.erase(
        std::remove_if(
            m_dependencies.begin(), m_dependencies.end(),
            [this](const RDGDependency& edge) {
                return !static_cast<const RDGPassNode*>(GetNodeBaseById(edge.source))->live ||
                    !static_cast<const RDGPassNode*>(GetNodeBaseById(edge.destination))->live;
            }),
        m_dependencies.end());
    HashMap<uint64_t, bool> edges;

    for (RDGDependency const& dependency : m_dependencies)
    {
        edges.emplace(CreateNodePairKey(dependency.source, dependency.destination), true);
    }

    m_compileStats.dependencyEdgeCount = uint32_t(edges.size());

    for (RDGResourceManager::Allocation* resource : m_resourceManager.m_resources)
    {
        resource->liveAccessCount = 0;
        resource->firstUse        = UINT32_MAX;
        resource->lastUse         = 0;
    }

    for (uint32_t order = 0; order < m_sortedNodes.size(); ++order)
    {
        const RDGNodeBase* node = GetNodeBaseById(m_sortedNodes[order]);

        for (uint32_t i = 0; i < node->accessCount; ++i)
        {
            RDGResourceManager::Allocation* resource =
                m_resourceManager.m_resources[m_accesses[node->accessOffset + i].resourceId];
            ++resource->liveAccessCount;
            resource->firstUse = std::min(resource->firstUse, order);
            resource->lastUse  = order;
        }
    }
}

void RenderGraph::BuildCompiledNodeList()
{
    m_compiledNodes.clear();
    const uint32_t edgeCount           = m_compileStats.dependencyEdgeCount;
    m_compileStats                     = {};
    m_compileStats.dependencyEdgeCount = edgeCount;
    m_compiledNodes.reserve(m_nodeCount);

    for (RDG_ID id : m_sortedNodes)
    {
        m_compiledNodes.emplace_back().nodeId = id;
    }

    m_compileStats.nodeCount       = uint32_t(m_sortedNodes.size());
    m_compileStats.passCount       = uint32_t(m_sortedNodes.size());
    m_compileStats.culledPassCount = m_nodeCount - uint32_t(m_sortedNodes.size());

    for (const RDGResourceManager::Allocation* resource : m_resourceManager.m_resources)
    {
        m_compileStats.liveResourceCount += resource->liveAccessCount != 0;
    }

    m_compileStats.resourceCount = static_cast<uint32_t>(m_resourceManager.m_resources.size());
}

bool RenderGraph::ValidateCompiledGraph()
{
    bool valid = true;

    valid = Check(m_nodes.size() == m_nodeCount && m_compiledNodes.size() == m_sortedNodes.size(),
                  RDGErrorCode::eBinding, "RDG compiled node count mismatch");

    if (valid)
    {
        for (const RDGNodeBase* base : m_nodes)
        {
            const RDGPassNode* node = static_cast<const RDGPassNode*>(base);

            if (!node->live)
            {
                continue;
            }

            valid = Check(node->pCompiledPass != nullptr, RDGErrorCode::eAllocation,
                          "Uncompiled RDG pass");
            valid = valid &&
                (Check(uint64_t(node->accessOffset) + node->accessCount <= m_accesses.size(),
                       RDGErrorCode::eBinding, "Invalid access slice"));

            if (valid)
            {
                for (uint32_t i = 0; i < node->accessCount; ++i)
                {
                    RDGAccess const& access = m_accesses[node->accessOffset + i];
                    valid =
                        Check(access.nodeId == node->id &&
                                  m_resourceManager.FindResourceByIdx(access.resourceId) != nullptr,
                              RDGErrorCode::eBinding, "Invalid RDG resource access");

                    if (!valid)
                    {
                        break;
                    }
                }
            }

            if (!valid)
            {
                break;
            }
        }
    }

    return valid;
}

bool RenderGraph::CanExecuteOnTransferQueue(const ResourceStateTracker& tracker) const
{
    bool valid = true;

    for (RDGCompiledNode const& compiled : m_compiledNodes)
    {
        const RDGNodeBase* node = GetNodeBaseById(compiled.nodeId);
        valid = !(node->type != RDGNodeType::eTransferPass || !TransferStages(node->selfStages) ||
                  !TransferStages(compiled.prologueSrcStages) ||
                  !TransferStages(compiled.prologueDstStages));

        if (valid)
        {
            const RDGPassNode* passNode = static_cast<const RDGPassNode*>(node);
            valid = !(static_cast<const RDGTransferPass*>(passNode->pCompiledPass)
                          ->requiresGraphicsQueue);

            if (valid)
            {
                for (RDGAccess const& access : compiled.initialResourceAccesses)
                {
                    const RDGResourceManager::Allocation* resource =
                        m_resourceManager.FindResourceByIdx(access.resourceId);
                    BitField<RHIPipelineStageFlagBits> stages =
                        resource->type == RDGResourceType::eTexture ?
                        tracker.GetTextureState(resource->pTexture).pipelineStages :
                        tracker.GetBufferState(resource->pBuffer).pipelineStages;
                    valid = TransferStages(stages);

                    if (!valid)
                    {
                        break;
                    }
                }
            }
        }

        if (!valid)
        {
            break;
        }
    }

    return valid;
}

void RenderGraph::EmitCompiledNodeBarriers(RDGCompiledNode& compiled, ResourceStateTracker& tracker)
{
    // CompileGraph refreshes every prologue from persistent state before each execution.
    // Do not reconstruct barriers from the latest reader and lose the retained writer scope.
    HeapVector<RHIBufferTransition>& buffers   = compiled.prologueBufferTransitions;
    HeapVector<RHITextureTransition>& textures = compiled.prologueTextureTransitions;
    m_recordedInitBarrierCount += compiled.initialBarrierCount;

    if (!buffers.empty() || !textures.empty())
    {
        m_pCmdList->AddTransitions(compiled.prologueSrcStages, compiled.prologueDstStages, {},
                                   buffers, textures);
    }

    if (m_activeMetrics != nullptr)
    {
        m_activeMetrics->ObserveBarriers(*this, compiled, tracker, compiled.prologueSrcStages,
                                         compiled.prologueDstStages, buffers, textures,
                                         compiled.initialBarrierCount);
    }
}

void RenderGraph::UpdateResourceStatesForNodeAccesses(const RDGCompiledNode& compiled,
                                                      ResourceStateTracker& tracker)
{
    const RDGNodeBase* node = GetNodeBaseById(compiled.nodeId);

    for (uint32_t i = 0; i < node->accessCount; ++i)
    {
        RDGAccess const& access = m_accesses[node->accessOffset + i];
        const RDGResourceManager::Allocation* resource =
            m_resourceManager.FindResourceByIdx(access.resourceId);

        if (resource->type == RDGResourceType::eTexture)
        {
            tracker.SetTextureState(resource->pTexture,
                                    AdvanceState(tracker.GetTextureState(resource->pTexture),
                                                 access, compiled.prologueDstStages));
        }
        else
        {
            tracker.SetBufferState(resource->pBuffer,
                                   AdvanceState(tracker.GetBufferState(resource->pBuffer), access,
                                                compiled.prologueDstStages));
        }
    }
}

bool RenderGraph::Execute(RHICommandList* cmdList, ResourceStateTracker& tracker)
{
    bool valid = true;

    valid = Check(m_executionState == RDGExecutionState::eCompiled, RDGErrorCode::eLifecycle,
                  "Graph is not compiled");

    if (valid)
    {
        m_pCmdList                 = cmdList;
        m_executionState           = RDGExecutionState::eExecuting;
        m_recordedInitBarrierCount = 0;

        for (RDGCompiledNode& compiled : m_compiledNodes)
        {
            if (m_activeMetrics != nullptr)
            {
                m_activeMetrics->BeginNode(*this, compiled);
            }

            EmitCompiledNodeBarriers(compiled, tracker);
            valid = RunNode(GetNodeBaseById(compiled.nodeId));

            if (valid)
            {
                UpdateResourceStatesForNodeAccesses(compiled, tracker);

                if (m_activeMetrics != nullptr)
                {
                    m_activeMetrics->EndNode();
                }
            }

            if (!valid)
            {
                break;
            }
        }
    }

    if (valid)
    {
        m_pCmdList       = nullptr;
        m_executionState = RDGExecutionState::eCompiled;
    }

    return valid;
}

bool RenderGraph::RunNode(RDGNodeBase* base)
{
    bool result = false;

    RDGPassNode* node = static_cast<RDGPassNode*>(base);
    m_currentNode     = node->id;

    switch (node->type)
    {
        case RDGNodeType::eGraphicsPass:
            result = ExecuteGraphicsPass(static_cast<RDGGraphicsPass*>(node->pCompiledPass),
                                         node->cmdLambdaIdx);
            break;

        case RDGNodeType::eComputePass:
            result = ExecuteComputePass(node->pCompiledPass, node->cmdLambdaIdx);
            break;
        case RDGNodeType::eTransferPass:
            result = ExecuteTransferPass(static_cast<RDGTransferPass*>(node->pCompiledPass),
                                         node->cmdLambdaIdx);
            break;

        default: result = Fail(RDGErrorCode::eBinding, "Unknown RDG node type"); break;
    }

    return result;
}

bool RenderGraph::ExecuteGraphicsPass(RDGGraphicsPass* pass, uint32_t lambdaIndex)
{
    bool valid = true;

    valid = Check(pass != nullptr && pass->pRenderingLayout != nullptr, RDGErrorCode::eAllocation,
                  "Invalid graphics pass");

    if (valid)
    {
        m_pCmdList->BeginRendering(pass->pRenderingLayout);
        m_pCmdList->BindPipeline(RHIPipelineType::eGraphics, pass->pPipeline);
        m_pCmdList->SetShaderParameters(pass->shaderParameters);
        const Rect2<int>& area = pass->pRenderingLayout->renderArea;
        m_pCmdList->SetViewport(area.minX, area.minY, area.maxX, area.maxY);
        m_pCmdList->SetScissor(area.minX, area.minY, area.maxX, area.maxY);

        if (!pass->geometryBuffer.vertexBuffers.empty())
        {
            HeapVector<uint64_t> offsets(pass->geometryBuffer.vertexBuffers.size());
            m_pCmdList->BindVertexBuffers(pass->geometryBuffer.vertexBuffers, offsets);
        }

        if (lambdaIndex < m_passCmdLambdas.size())
        {
            RDGPassCmdEncoder encoder(
                m_pCmdList, pass,
                m_activeMetrics && m_activeMetrics->m_capture ? &m_activeMetrics->m_node : nullptr,
                &m_pendingGfxPassDescs[static_cast<RDGPassNode*>(GetNodeBaseById(m_currentNode))
                                           ->passDescIdx]);
            m_passCmdLambdas[lambdaIndex](encoder);
            const RDGResult& result = encoder.GetResult();

            if (!result)
            {
                valid = Fail(result.code,
                             "Pass '" + GetNodeBaseById(m_currentNode)->tag.ToString() +
                                 "': " + result.message);
            }

            valid = valid && (m_result);
        }

        if (valid)
        {
            m_pCmdList->EndRendering();
        }
    }

    return valid;
}

bool RenderGraph::ExecuteComputePass(RDGCompiledPass* compiled, uint32_t lambdaIndex)
{
    bool valid = true;

    RDGComputePass* pass = static_cast<RDGComputePass*>(compiled);
    m_pCmdList->BindPipeline(RHIPipelineType::eCompute, pass->pPipeline);
    m_pCmdList->SetShaderParameters(pass->shaderParameters);

    if (lambdaIndex < m_passCmdLambdas.size())
    {
        RDGPassCmdEncoder encoder(
            m_pCmdList, pass,
            m_activeMetrics && m_activeMetrics->m_capture ? &m_activeMetrics->m_node : nullptr,
            &m_pendingComputePassDescs[static_cast<RDGPassNode*>(GetNodeBaseById(m_currentNode))
                                           ->passDescIdx]);
        m_passCmdLambdas[lambdaIndex](encoder);
        const RDGResult& result = encoder.GetResult();

        if (!result)
        {
            valid = Fail(result.code,
                         "Pass '" + GetNodeBaseById(m_currentNode)->tag.ToString() +
                             "': " + result.message);
        }

        valid = valid && (m_result);
    }

    return valid;
}

bool RenderGraph::ExecuteTransferPass(RDGTransferPass*, uint32_t lambdaIndex)
{
    bool valid = true;

    if (lambdaIndex < m_passCmdLambdas.size())
    {
        RDGPassCmdEncoder encoder(
            m_pCmdList, nullptr,
            m_activeMetrics && m_activeMetrics->m_capture ? &m_activeMetrics->m_node : nullptr);
        m_passCmdLambdas[lambdaIndex](encoder);
        const RDGResult& result = encoder.GetResult();

        if (!result)
        {
            valid = Fail(result.code,
                         "Pass '" + GetNodeBaseById(m_currentNode)->tag.ToString() +
                             "': " + result.message);
        }

        valid = valid && (m_result);
    }

    return valid;
}

void RenderGraph::DestroyNode(RDGNodeBase* node)
{
    static_cast<RDGPassNode*>(node)->~RDGPassNode();
}

RDGGraphicsPass* RenderGraph::AcquireGraphicsPass()
{
    RDGGraphicsPass* result{};

    if (m_idleGfxPasses.empty())
    {
        result = ZEN_NEW() RDGGraphicsPass();
    }
    else
    {
        RDGGraphicsPass* pass = m_idleGfxPasses.back();
        m_idleGfxPasses.pop_back();
        m_idlePassBytes -= pass->GetStorageBytes();
        result = pass;
    }

    return result;
}

RDGComputePass* RenderGraph::AcquireComputePass()
{
    RDGComputePass* result{};

    if (m_idleComputePasses.empty())
    {
        result = ZEN_NEW() RDGComputePass();
    }
    else
    {
        RDGComputePass* pass = m_idleComputePasses.back();
        m_idleComputePasses.pop_back();
        m_idlePassBytes -= pass->GetStorageBytes();
        result = pass;
    }

    return result;
}

void RenderGraph::TrimCompiledPassStorage()
{
    for (RDGGraphicsPass* pass : m_idleGfxPasses)
    {
        ZEN_DELETE(pass);
    }

    for (RDGComputePass* pass : m_idleComputePasses)
    {
        ZEN_DELETE(pass);
    }

    m_idleGfxPasses.clear();
    m_idleComputePasses.clear();
    m_idlePassBytes = 0;
}

template <typename Pass> void RenderGraph::ReleaseCompiledPass(Pass* pass, HeapVector<Pass*>& idle)
{
    if (m_pRenderDevice != nullptr)
    {
        m_pRenderDevice->DeferDestroyPipeline(pass->pPipeline);
    }

    pass->pPipeline = nullptr;
    pass->passTag   = {};
    pass->shaderParameters.Reset();
    pass->indirectBindings.clear();
    const size_t bytes = pass->GetStorageBytes();

    if (m_idleGfxPasses.size() + m_idleComputePasses.size() < cMaxIdlePassCount &&
        bytes <= cMaxIdlePassBytes - m_idlePassBytes)
    {
        m_idlePassBytes += bytes;
        idle.push_back(pass);
    }
    else
    {
        ZEN_DELETE(pass);
    }
}

void RenderGraph::ReleaseCompiledShaderPasses()
{
    // Reverse release preserves recording-order storage reuse for similar consecutive frames.
    for (size_t i = m_compiledGfxPasses.size(); i > 0; --i)
    {
        RDGGraphicsPass* pass = m_compiledGfxPasses[i - 1];

        if (m_pRenderDevice != nullptr)
        {
            m_pRenderDevice->ReleaseRenderingLayout(pass->pRenderingLayout);
        }

        pass->pRenderingLayout = nullptr;
        pass->geometryBuffer.vertexBuffers.clear();
        pass->geometryBuffer.pIndexBuffer      = nullptr;
        pass->geometryBuffer.indexBufferFormat = DataFormat::eR32UInt;
        pass->geometryBuffer.indexBufferOffset = 0;
        ReleaseCompiledPass(pass, m_idleGfxPasses);
    }

    for (size_t i = m_compiledComputePasses.size(); i > 0; --i)
    {
        ReleaseCompiledPass(m_compiledComputePasses[i - 1], m_idleComputePasses);
    }

    m_compiledGfxPasses.clear();
    m_compiledComputePasses.clear();

    for (RDGNodeBase* base : m_nodes)
    {
        RDGPassNode* node = static_cast<RDGPassNode*>(base);

        if (node->type != RDGNodeType::eTransferPass)
        {
            node->pCompiledPass = nullptr;
        }
    }
}

void RenderGraph::ResetBuildState()
{
    if (m_inExecution)
    {
        Fail(RDGErrorCode::eLifecycle, "Cannot reset an executing graph");
        return;
    }

    ++m_buildGeneration;
    m_openTransferRecorders = 0;
    ReleaseCompiledShaderPasses();

    for (RDGTransferPass* pass : m_compiledXferPasses)
    {
        ZEN_DELETE(pass);
    }

    m_compiledGfxPasses.clear();
    m_compiledComputePasses.clear();
    m_compiledXferPasses.clear();
    m_pendingGfxPassDescs.clear();
    m_pendingComputePassDescs.clear();
    m_pendingTransferPassDescs.clear();
    m_passCmdLambdas.clear();

    for (RDGNodeBase* node : m_nodes)
    {
        DestroyNode(node);
    }

    m_nodes.clear();
    m_resourceManager.ReleaseTransientResources();
    m_resourceManager.Reset();
    m_poolAlloc.Reset();
    m_compiledNodes.clear();
    m_sortedNodes.clear();
    m_accesses.clear();
    m_pendingContentWarnings.clear();
    m_warnings.clear();
    m_finalContents.clear();
    m_transientRTMap.clear();
    m_inDegrees.clear();
    m_dependencies.clear();
    m_nodeCount      = 0;
    m_pCmdList       = nullptr;
    m_compileStats   = {};
    m_executionState = RDGExecutionState::eIdle;
}

void RenderGraph::Destroy()
{
    ResetBuildState();
    TrimCompiledPassStorage();
    m_resourceManager.Destroy(m_pRenderDevice);
}

bool RenderGraph::Reset()
{
    bool result{};

    if (m_inExecution)
    {
        result = Fail(RDGErrorCode::eLifecycle, "Cannot reset an executing graph");
    }
    else
    {
        ResetBuildState();
        m_result = {};
        result   = true;
    }

    return result;
}

bool RenderGraph::Begin()
{
    bool result{};

    if (m_executionState == RDGExecutionState::eBuilding || m_inExecution)
    {
        result =
            Fail(RDGErrorCode::eLifecycle, "RenderGraph::Begin called twice or during execution");
    }
    else
    {
        ResetBuildState();
        m_result         = {};
        m_executionState = RDGExecutionState::eBuilding;
        result           = true;
    }

    return result;
}

bool RenderGraph::End()
{
    bool result{};

    if (m_result)
    {
        if (m_executionState != RDGExecutionState::eBuilding)
        {
            result = Fail(RDGErrorCode::eLifecycle, "RenderGraph::End requires Begin");
        }
        else if (m_openTransferRecorders != 0)
        {
            result =
                Fail(RDGErrorCode::eLifecycle, "End requires transfer recorders to leave scope");
        }
        else if (!m_resourceManager.DeclareExtractions() || !FinalizeResourceVersions())
        {
            result = false;
        }
        else
        {
            uint64_t accessCount = 0;

            for (const RDGNodeBase* node : m_nodes)
            {
                accessCount += node->accessCount;
            }

            if (!Check(accessCount <= std::numeric_limits<uint32_t>::max(), RDGErrorCode::eRange,
                       "RenderGraph has too many resource accesses"))
            {
                result = false;
            }
            else
            {
                m_accesses.reserve(static_cast<size_t>(accessCount));

                for (RDGNodeBase* base : m_nodes)
                {
                    RDGPassNode* node  = static_cast<RDGPassNode*>(base);
                    node->accessOffset = static_cast<uint32_t>(m_accesses.size());

                    for (RDGAccess const& access : node->pendingAccesses)
                    {
                        m_accesses.push_back(access);
                    }

                    node->pendingAccesses =
                        {}; // Release recording storage after its one-time flatten.
                }

                m_executionState = RDGExecutionState::eRecorded;
                result           = true;
            }
        }
    }

    return result;
}
} // namespace zen::rc
