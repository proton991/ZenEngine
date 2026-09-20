#pragma once
#include "RDGDefs.h"
#include "Templates/HeapVector.h"

namespace zen::rc
{
// Logical placement only. RenderDevice owns command contexts and native submission.
enum class RDGQueue : uint8_t
{
    eGraphics,
    eAsyncCompute,
    eTransfer,
    eCount
};

inline const char* RDGQueueName(RDGQueue queue)
{
    const char* name = "graphics";
    if (queue == RDGQueue::eAsyncCompute)
    {
        name = "compute";
    }
    else if (queue == RDGQueue::eTransfer)
    {
        name = "transfer";
    }
    return name;
}

struct RDGScheduledPass
{
    RDG_ID nodeId;
    RDGQueuePreference preference{RDGQueuePreference::eDefault};
    RDGAsyncComputeEligibility eligibility{RDGAsyncComputeEligibility::eNotRequested};
};

// RenderDevice supplies provenance for an accepted initial state. The dependency ID refers
// to its owned producer point; RDG never inspects or guesses native submission serials.
struct RDGExternalQueueState
{
    uint64_t resourceId{0}; // Physical stable ID, with texture views normalized to their image.
    RDGQueue queue{RDGQueue::eGraphics};
    uint32_t dependencyId{UINT32_MAX};
    // History can supply multiple queue-local accesses instead of one ordered tracker state.
    bool hasAccessState{false};
    RDGAccess access;
    bool ordersReaders{true}; // Writer/layout transition; plain readers only constrain overwrites.
};

struct RDGScheduledResource
{
    RDGAccess firstAccess;
    RDGAccess lastAccess;
    BitField<RHIAccessFlagBits> accessFlags;
    BitField<RHIPipelineStageFlagBits> stages;
    bool reads{false};
    bool writes{false};
};

// Logical synchronization requirements, not directly executable native barriers.
// An invalid source.nodeId denotes the supplied external/initial resource state.
struct RDGScheduleBoundary
{
    RDGAccess source;
    RDGAccess destination;
};

struct RDGSubmissionGroup
{
    uint32_t id{0};
    RDGQueue queue{RDGQueue::eGraphics};
    uint32_t queueEquivalenceId{0};
    HeapVector<RDGScheduledPass> passes;
    HeapVector<RDGScheduledResource> resources;
    HeapVector<uint32_t> predecessors;
    // Exact group references; resolved to accepted producer serials at submission time.
    // Native queue aliases retain predecessors but require ordinary barriers instead of waits.
    HeapVector<uint32_t> semaphorePredecessors;
    HeapVector<uint32_t> externalPredecessors;
    HeapVector<uint32_t> externalSemaphorePredecessors;
    BitField<RHIPipelineStageFlagBits> waitStages{RHIPipelineStageFlagBits::eAllCommands};
    HeapVector<RDGScheduleBoundary> boundaries;
};

struct RDGSchedule
{
    // Groups are in a stable topological order; all predecessor IDs are smaller.
    HeapVector<RDGSubmissionGroup> groups;
    HeapVector<RDGDependency> dependencies;
    bool usesMultipleQueues{false};
    bool allowsAllocationReuse{true};
};
} // namespace zen::rc
