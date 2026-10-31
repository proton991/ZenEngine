#pragma once
#include "RenderGraph.h"

namespace zen::rc
{
enum class RDGDebugExecutionMode
{
    Graphics,
    Grouped
};

struct RDGDebugPass
{
    int32_t                      id{-1};
    std::string                  name;
    RDGNodeType                  type{RDGNodeType::eNone};
    bool                         live{false};
    uint32_t                     order{UINT32_MAX};
    RDGQueuePreference           preference{RDGQueuePreference::eDefault};
    RDGAsyncComputeEligibility   eligibility{RDGAsyncComputeEligibility::eNotRequested};
    RHICommandContextType        queue{RHICommandContextType::eGraphics};
    uint32_t                     group{UINT32_MAX};
    HeapVector<RDGAccess>        accesses;
    HeapVector<RDGVersionAccess> versions;
};

struct RDGDebugResource
{
    int32_t              id{-1};
    std::string          name;
    RDGResourceType      type{RDGResourceType::eNone};
    RDGResourceLifeCycle lifecycle{RDGResourceLifeCycle::eTransient};
    TextureFormat        texture;
    uint64_t             bufferSize{0};
    uint32_t             usageFlags{0};
};

struct RDGDebugVersion
{
    int32_t  resource{-1};
    int32_t  previous{-1};
    uint32_t number{0};
};

// Owns all metadata. No graph, allocation, GPU resource, or command-list pointers.
struct RDGDebugSnapshot
{
    std::string                    graph;
    uint64_t                       generation{0};
    uint64_t                       frame{0};
    bool                           accepted{false};
    RDGDebugExecutionMode          mode{RDGDebugExecutionMode::Graphics};
    RDGCompileStats                stats;
    HeapVector<std::string>        warnings;
    HeapVector<RDGDebugPass>       passes;
    HeapVector<RDGDebugResource>   resources;
    HeapVector<RDGDebugVersion>    versions;
    HeapVector<RDGDependency>      dependencies;
    HeapVector<RDGSubmissionGroup> groups;

    static RDGDebugSnapshot Capture(const RenderGraph&                 graph,
                                    const RDGSchedule&                 schedule,
                                    const HeapVector<RDGCompiledNode>& nodes,
                                    RDGDebugExecutionMode              mode);
};
} // namespace zen::rc
