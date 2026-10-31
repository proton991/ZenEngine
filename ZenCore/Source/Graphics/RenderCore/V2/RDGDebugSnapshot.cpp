#include "Graphics/RenderCore/V2/RenderGraph/RDGDebugSnapshot.h"
#include "Graphics/RenderCore/V2/RenderFrameState.h"

namespace zen::rc
{
RDGDebugSnapshot RDGDebugSnapshot::Capture(const RenderGraph&                 graph,
                                           const RDGSchedule&                 schedule,
                                           const HeapVector<RDGCompiledNode>& nodes,
                                           RDGDebugExecutionMode              mode)
{
    RDGDebugSnapshot result;
    result.graph        = graph.m_rdgTag.CStr();
    result.generation   = graph.m_buildGeneration;
    result.frame        = ToValue(GRenderFrameState.GetFrameNumber());
    result.mode         = mode;
    result.stats        = graph.m_compileStats;
    result.dependencies = schedule.dependencies;
    if (mode == RDGDebugExecutionMode::Grouped)
    {
        result.groups = schedule.groups;
    }
    for (const RDGResult& warning : graph.m_warnings)
    {
        result.warnings.push_back(warning.message);
    }
    for (const RDGNodeBase* base : graph.m_nodes)
    {
        const RDGPassNode& node = *static_cast<const RDGPassNode*>(base);
        RDGDebugPass       pass;
        pass.id         = node.id;
        pass.name       = node.tag.CStr();
        pass.type       = node.type;
        pass.live       = node.live;
        pass.preference = node.queuePreference;
        pass.versions   = node.versionAccesses;
        for (uint32_t i = 0; i < node.accessCount; ++i)
        {
            pass.accesses.push_back(graph.m_accesses[node.accessOffset + i]);
        }
        result.passes.push_back(std::move(pass));
    }
    for (uint32_t order = 0; order < nodes.size(); ++order)
    {
        const RDGCompiledNode& node = nodes[order];
        if (node.nodeId >= 0 && size_t(node.nodeId) < result.passes.size())
        {
            RDGDebugPass& pass = result.passes[node.nodeId];
            pass.order         = order;
            pass.eligibility   = node.asyncComputeEligibility;
            pass.queue         = mode == RDGDebugExecutionMode::Grouped ? node.plannedQueue : RHICommandContextType::eGraphics;
            pass.group         = mode == RDGDebugExecutionMode::Grouped ? node.submissionGroup : UINT32_MAX;
        }
    }
    for (const RDGResourceManager::Allocation* allocation : graph.m_resourceManager.m_resources)
    {
        RDGDebugResource resource;
        resource.id         = allocation->id;
        resource.name       = allocation->name.CStr();
        resource.type       = allocation->type;
        resource.lifecycle  = allocation->GetLifeCycle();
        resource.texture    = allocation->texFormat;
        resource.bufferSize = allocation->bufferSize;
        resource.usageFlags = uint32_t(int64_t(allocation->usageFlags));
        result.resources.push_back(std::move(resource));
    }
    for (const RDGResourceManager::ResourceVersion& version : graph.m_resourceManager.m_versions)
    {
        result.versions.push_back({version.resourceId, version.previous, version.number});
    }
    return result;
}
} // namespace zen::rc
