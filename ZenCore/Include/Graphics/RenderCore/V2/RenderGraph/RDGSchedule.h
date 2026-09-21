#pragma once
#include "RDGDefs.h"
#include "Graphics/RHI/RHICommandList.h"
#include "Templates/HeapVector.h"

namespace zen::rc
{
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
    RHICommandContextType queue{RHICommandContextType::eGraphics};
    uint32_t dependencyId{UINT32_MAX};
    // History can supply multiple queue-local accesses instead of one ordered tracker state.
    bool hasAccessState{false};
    RDGAccess access;
    bool ordersReaders{true}; // Writer/layout transition; plain readers only constrain overwrites.
};

struct RDGSubmissionGroup
{
    uint32_t id{0};
    RHICommandContextType queue{RHICommandContextType::eGraphics};
    uint32_t queueEquivalenceId{0};
    HeapVector<RDGScheduledPass> passes;
    HeapVector<uint32_t> predecessors;
    // Exact group references; resolved to accepted producer serials at submission time.
    // Native queue aliases retain predecessors but require ordinary barriers instead of waits.
    HeapVector<uint32_t> externalPredecessors;
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
